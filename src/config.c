#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/mem.h"
#include "config_yaml.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal auto-cleanup for config handles used inside this file. */
static void config_free_p(cutils_config_t **p)
{
    if (*p) {
        config_free(*p);
        *p = NULL;
    }
}
#define CUTILS_AUTO_CONFIG __attribute__((cleanup(config_free_p)))

/* Sorted-by-key index entry. Pairs a key string with its position in
 * cfg->keys / cfg->env_values so a bsearch on `key` lands us at the
 * registration-order index needed to read the value. This replaces a
 * 1.0.x linear scan over cfg->keys that, for a 300-key registry,
 * dominated every config_get_str at ~1.9 µs per call. With bsearch
 * the same lookup drops to log2(300) ≈ 8 strcmps. */
typedef struct {
    const char *key;
    int         idx;
} sorted_key_entry_t;

/* --- c-utils internal keys --- */

/* Hard minimum: required for c-utils to function, file-only, immutable */
static const config_key_t cutils_internal_keys[] = {
    { "db.path", CFG_STRING, "app.db", "SQLite database file path",
      CFG_STORE_FILE, 1 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

/* Internal sections */
static const config_section_t cutils_internal_sections[] = {
    { "db", "Database" },
    { NULL, NULL }
};

/* --- Config handle --- */

/* Per-key cache of DB-fetched values. One entry per key ever read; the
 * list grows monotonically through the config's lifetime. Gives each key
 * its own stable buffer so callers can hold `const char *` across other
 * config_get_* calls — the original shared-buffer design silently
 * aliased pointers when two DB-backed keys were read in sequence (see
 * db_val_cache_get below for the mechanism). */
typedef struct db_val_cache {
    char                *key;
    char                *value;  /* strdup'd from DB result */
    struct db_val_cache *next;
} db_val_cache_t;

struct cutils_config {
    char           *app_name;      /* for env var prefix */
    char           *env_prefix;    /* uppercase: "AIRIES_UPS" */
    char           *config_path;

    /* Merged key registry (internal + app) */
    config_key_t   *keys;
    /* Parallel to `keys`: captured env-var value at the time the key was
     * registered, or NULL if the env var was unset. Snapshotting at
     * registration eliminates per-read getenv() (one syscall removed
     * from the read path) and closes the POSIX hazard where a
     * concurrent setenv on any thread invalidates the previous getenv
     * return pointer. The documented contract is "no setenv after
     * appguard_init returns"; with this snapshot we no longer rely on
     * that assumption holding — env changes simply aren't observed,
     * which matches the documented behavior. */
    char          **env_values;
    int             nkeys;
    int             keys_capacity;

    /* Sorted-by-key view of `keys` for O(log n) lookups. Rebuilt by
     * sorted_keys_rebuild after each registration phase (config_init's
     * tail and config_attach_db's tail). Reads use bsearch through
     * this index; the looked-up idx maps back into cfg->keys and
     * cfg->env_values. Allocated to cfg->nkeys slots — exactly one
     * entry per key. */
    sorted_key_entry_t *sorted_keys;
    int                 sorted_keys_cap;

    /* Merged sections */
    config_section_t *sections;
    int               nsections;
    int               sections_capacity;

    /* Parsed YAML document */
    yaml_doc_t     *doc;

    /* DB handle (attached after DB init, NULL until then) */
    cutils_db_t    *db;

    /* Per-key cache for DB-fetched values — see db_val_cache_t comment.
     * Held through one level of indirection so a `const cutils_config_t *`
     * caller can still mutate `*db_cache_head` (prepend a slot) without
     * needing a const-cast on cfg: writing through the inner pointer
     * doesn't modify the struct, it modifies the heap memory the struct
     * points to. Allocated in config_init, freed in config_free. */
    db_val_cache_t **db_cache_head;

    /* Serializes every public read/write entry point so concurrent
     * callers don't tear the keys array, the YAML doc, or the DB
     * cache list. Recursive so config_get_int / config_get_bool can
     * call config_get_str under the same lock without self-deadlocking.
     *
     * Heap-allocated through a pointer so callers holding a
     * `const cutils_config_t *` (read API) can lock without a
     * const-cast on the mutex itself — the pointee is non-const even
     * when the pointer-in-cfg is const-propagated. */
    pthread_mutex_t *mutex;
};

/* --- Helpers --- */

static char *make_env_prefix(const char *app_name)
{
    size_t len = strlen(app_name);
    char *prefix = malloc(len + 2);
    if (!prefix) return NULL;

    for (size_t i = 0; i < len; i++) {
        if (app_name[i] == '-' || app_name[i] == '.')
            prefix[i] = '_';
        else
            prefix[i] = (char)toupper((unsigned char)app_name[i]);
    }
    prefix[len] = '_';
    prefix[len + 1] = '\0';
    return prefix;
}

static char *make_env_name(const char *prefix, const char *dotkey)
{
    size_t plen = strlen(prefix);
    size_t klen = strlen(dotkey);
    char *name = malloc(plen + klen + 1);
    if (!name) return NULL;

    memcpy(name, prefix, plen);
    for (size_t i = 0; i < klen; i++) {
        if (dotkey[i] == '.')
            name[plen + i] = '_';
        else
            name[plen + i] = (char)toupper((unsigned char)dotkey[i]);
    }
    name[plen + klen] = '\0';
    return name;
}

/* Snapshot the env-var value for keys[idx] into env_values[idx].
 * Returns CUTILS_OK on success (including "no env var set" — stored as NULL)
 * and CUTILS_ERR_NOMEM if a strdup or the env-name buffer alloc fails. */
static int capture_env_for_key(cutils_config_t *cfg, int idx)
{
    cfg->env_values[idx] = NULL;
    if (!cfg->env_prefix) return CUTILS_OK;

    CUTILS_AUTOFREE char *envname = make_env_name(cfg->env_prefix,
                                                  cfg->keys[idx].key);
    if (!envname)
        return set_error(CUTILS_ERR_NOMEM, "config env name alloc");

    const char *v = getenv(envname);
    if (!v) return CUTILS_OK;

    cfg->env_values[idx] = strdup(v);
    if (!cfg->env_values[idx])
        return set_error(CUTILS_ERR_NOMEM, "config env value alloc");
    return CUTILS_OK;
}

static int add_key(cutils_config_t *cfg, const config_key_t *key)
{
    /* Check for collision */
    for (int i = 0; i < cfg->nkeys; i++) {
        if (strcmp(cfg->keys[i].key, key->key) == 0)
            return set_error(CUTILS_ERR_EXISTS,
                "config key '%s' registered twice", key->key);
    }

    if (cfg->nkeys >= cfg->keys_capacity) {
        int newcap = cfg->keys_capacity ? cfg->keys_capacity * 2 : 32;
        config_key_t *tmp = realloc(cfg->keys,
                                    (size_t)newcap * sizeof(config_key_t));
        if (!tmp) return set_error(CUTILS_ERR_NOMEM, "config key alloc");
        cfg->keys = tmp;

        char **etmp = realloc(cfg->env_values,
                              (size_t)newcap * sizeof(char *));
        if (!etmp) return set_error(CUTILS_ERR_NOMEM, "config env_values alloc");
        cfg->env_values = etmp;

        cfg->keys_capacity = newcap;
    }

    cfg->keys[cfg->nkeys] = *key;
    int rc = capture_env_for_key(cfg, cfg->nkeys);
    if (rc != CUTILS_OK) return rc;
    cfg->nkeys++;
    return CUTILS_OK;
}

static int add_section(cutils_config_t *cfg, const config_section_t *sec)
{
    /* Skip duplicates silently */
    for (int i = 0; i < cfg->nsections; i++) {
        if (strcmp(cfg->sections[i].prefix, sec->prefix) == 0)
            return CUTILS_OK;
    }

    if (cfg->nsections >= cfg->sections_capacity) {
        int newcap = cfg->sections_capacity ? cfg->sections_capacity * 2 : 16;
        config_section_t *tmp = realloc(cfg->sections,
                                        (size_t)newcap * sizeof(config_section_t));
        if (!tmp) return set_error(CUTILS_ERR_NOMEM, "config section alloc");
        cfg->sections = tmp;
        cfg->sections_capacity = newcap;
    }

    cfg->sections[cfg->nsections++] = *sec;
    return CUTILS_OK;
}

static int is_internal_minimum(const char *key)
{
    for (int i = 0; cutils_internal_keys[i].key != NULL; i++) {
        if (strcmp(cutils_internal_keys[i].key, key) == 0)
            return 1;
    }
    return 0;
}

/* qsort/bsearch comparator on sorted_key_entry_t. Both call sites pass
 * sorted_key_entry_t* — for bsearch the search "key" is a stack-local
 * entry with only .key populated. */
static int sorted_key_cmp(const void *a, const void *b)
{
    const sorted_key_entry_t *ea = a;
    const sorted_key_entry_t *eb = b;
    return strcmp(ea->key, eb->key);
}

/* Rebuild the sorted-by-key view of cfg->keys. Called at the tail of
 * each registration phase; cheap (~O(n log n)) and one-shot.
 *
 * Returns CUTILS_OK or CUTILS_ERR_NOMEM. The previous sorted_keys
 * allocation is kept across calls and only grown when nkeys outpaces
 * its capacity — config_attach_db can append db keys after config_init
 * already ran, so the index must accommodate growth. */
static int sorted_keys_rebuild(cutils_config_t *cfg)
{
    if (cfg->nkeys > cfg->sorted_keys_cap) {
        int newcap = cfg->sorted_keys_cap ? cfg->sorted_keys_cap : 32;
        while (newcap < cfg->nkeys) newcap *= 2;
        sorted_key_entry_t *tmp = realloc(cfg->sorted_keys,
                                          (size_t)newcap * sizeof(*tmp));
        if (!tmp)
            return set_error(CUTILS_ERR_NOMEM, "sorted_keys alloc");
        cfg->sorted_keys     = tmp;
        cfg->sorted_keys_cap = newcap;
    }
    for (int i = 0; i < cfg->nkeys; i++) {
        cfg->sorted_keys[i].key = cfg->keys[i].key;
        cfg->sorted_keys[i].idx = i;
    }
    qsort(cfg->sorted_keys, (size_t)cfg->nkeys,
          sizeof(*cfg->sorted_keys), sorted_key_cmp);
    return CUTILS_OK;
}

/* Find the position of `key` in cfg->keys via bsearch over the sorted
 * view. Returns the index, or -1 if not found. */
static int find_key_idx(const cutils_config_t *cfg, const char *key)
{
    if (!cfg->sorted_keys || cfg->nkeys == 0) return -1;
    sorted_key_entry_t needle = { .key = key, .idx = 0 };
    const sorted_key_entry_t *hit = bsearch(
        &needle, cfg->sorted_keys, (size_t)cfg->nkeys,
        sizeof(*cfg->sorted_keys), sorted_key_cmp);
    return hit ? hit->idx : -1;
}

/* --- Init --- */

int config_init(cutils_config_t **out,
                const char *app_name,
                const char *config_path,
                config_first_run_t first_run,
                const config_key_t *file_keys,
                const config_section_t *sections)
{
    /* Every `return` below is covered by CUTILS_AUTO_CONFIG on `cfg`.
     * cppcheck does not model the cleanup attribute — suppress its
     * false-positive memleak warnings across this scope. */
    /* cppcheck-suppress-begin memleak */
    CUTILS_AUTO_CONFIG cutils_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return set_error(CUTILS_ERR_NOMEM, "config_init: alloc failed");

    /* Recursive so config_get_int / config_get_bool can take the lock
     * and then call config_get_str under the same hold. */
    cfg->mutex = malloc(sizeof(*cfg->mutex));
    if (!cfg->mutex)
        return set_error(CUTILS_ERR_NOMEM, "config_init: mutex alloc failed"); /* LCOV_EXCL_LINE */
    {
        pthread_mutexattr_t mattr;
        /* LCOV_EXCL_START — pthread_mutexattr_init / pthread_mutex_init
         * only fail on EAGAIN / EINVAL; not reachable here. */
        if (pthread_mutexattr_init(&mattr) != 0) {
            free(cfg->mutex);
            cfg->mutex = NULL;
            return set_error(CUTILS_ERR, "config_init: mutexattr init failed");
        }
        /* LCOV_EXCL_STOP */
        pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
        int mrc = pthread_mutex_init(cfg->mutex, &mattr);
        pthread_mutexattr_destroy(&mattr);
        /* LCOV_EXCL_START */
        if (mrc != 0) {
            free(cfg->mutex);
            cfg->mutex = NULL;
            return set_error(CUTILS_ERR, "config_init: mutex init failed");
        }
        /* LCOV_EXCL_STOP */
    }

    cfg->app_name = strdup(app_name);
    cfg->env_prefix = make_env_prefix(app_name);
    cfg->db_cache_head = calloc(1, sizeof(*cfg->db_cache_head));

    if (!cfg->app_name || !cfg->env_prefix || !cfg->db_cache_head)
        return set_error(CUTILS_ERR_NOMEM, "config_init: string alloc failed");

    /* Resolve the config file path. Precedence:
     *   1. Caller-supplied config_path (explicit intent wins).
     *   2. <APP>_CONFIG_PATH env var — lets containers and packages
     *      relocate the YAML without recompiling (Docker volumes,
     *      /etc/<pkg>/<pkg>.yaml in a .deb, etc.).
     *   3. Literal "config.yaml" in CWD (legacy behavior).
     * The path is interpreted by fopen, so relative paths still resolve
     * against CWD — the env var is for the absolute-path case. */
    const char *resolved = config_path;
    CUTILS_AUTOFREE char *env_name = NULL;
    if (!resolved) {
        size_t plen = strlen(cfg->env_prefix);
        env_name = malloc(plen + sizeof("CONFIG_PATH"));
        if (!env_name)
            return set_error(CUTILS_ERR_NOMEM,
                             "config_init: env name alloc failed");
        memcpy(env_name, cfg->env_prefix, plen);
        memcpy(env_name + plen, "CONFIG_PATH", sizeof("CONFIG_PATH"));
        const char *env_val = getenv(env_name);
        if (env_val && *env_val)
            resolved = env_val;
    }
    cfg->config_path = strdup(resolved ? resolved : "config.yaml");
    if (!cfg->config_path)
        return set_error(CUTILS_ERR_NOMEM,
                         "config_init: config_path alloc failed");

    /* Register internal keys and sections */
    for (int i = 0; cutils_internal_keys[i].key != NULL; i++) {
        int rc = add_key(cfg, &cutils_internal_keys[i]);
        if (rc != CUTILS_OK) return rc;
    }

    for (int i = 0; cutils_internal_sections[i].prefix != NULL; i++) {
        int rc = add_section(cfg, &cutils_internal_sections[i]);
        if (rc != CUTILS_OK) return rc;
    }

    /* Register app keys (file-backed only at this stage) */
    if (file_keys) {
        for (int i = 0; file_keys[i].key != NULL; i++) {
            if (file_keys[i].store != CFG_STORE_FILE) continue;
            int rc = add_key(cfg, &file_keys[i]);
            if (rc != CUTILS_OK) return rc;
        }
    }

    if (sections) {
        for (int i = 0; sections[i].prefix != NULL; i++) {
            int rc = add_section(cfg, &sections[i]);
            if (rc != CUTILS_OK) return rc;
        }
    }

    /* Check if config file exists */
    {
        CUTILS_AUTOCLOSE FILE *test = fopen(cfg->config_path, "r");
        if (!test) {
            /* Generate template */
            fprintf(stderr, "No config file found at '%s' — generating template.\n",
                    cfg->config_path);

            /* Only include file-backed keys in template */
            CUTILS_AUTOFREE config_key_t *fkeys =
                calloc((size_t)cfg->nkeys, sizeof(config_key_t));
            int nfkeys = 0;
            if (fkeys) {
                for (int i = 0; i < cfg->nkeys; i++) {
                    if (cfg->keys[i].store == CFG_STORE_FILE)
                        fkeys[nfkeys++] = cfg->keys[i];
                }
            }

            int rc = yaml_generate_template(cfg->config_path,
                                            fkeys, nfkeys,
                                            cfg->sections, cfg->nsections);
            if (rc != 0)
                return set_error(CUTILS_ERR_IO, "failed to write config template");

            if (first_run == CFG_FIRST_RUN_EXIT) {
                fprintf(stderr, "Config template written to '%s'. "
                        "Please review and restart.\n", cfg->config_path);
                *out = NULL;
                return CUTILS_ERR_NOT_FOUND;
            }

            /* CFG_FIRST_RUN_CONTINUE: fall through to parse the generated file */
        }
    }

    /* Parse the YAML file */
    cfg->doc = yaml_parse_file(cfg->config_path);
    if (!cfg->doc)
        return set_error(CUTILS_ERR_IO, "failed to parse config file '%s'",
                         cfg->config_path);

    /* Validate required keys */
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].store != CFG_STORE_FILE) continue;
        if (!cfg->keys[i].required) continue;

        const char *val = yaml_get(cfg->doc, cfg->keys[i].key);
        if (!val || val[0] == '\0')
            return set_error(CUTILS_ERR_CONFIG,
                "required config key '%s' is missing or empty", cfg->keys[i].key);
    }

    /* Build the sorted-by-key index now that registration is done.
     * Reads through find_key_idx land in O(log n). config_attach_db
     * runs another rebuild after appending its db keys. */
    int rc = sorted_keys_rebuild(cfg);
    if (rc != CUTILS_OK) return rc;

    /* Transfer ownership to caller; cleanup sees NULL and skips. */
    *out = CUTILS_MOVE(cfg);
    return CUTILS_OK;
    /* cppcheck-suppress-end memleak */
}

void config_free(cutils_config_t *cfg)
{
    if (!cfg) return;
    free(cfg->app_name);
    free(cfg->env_prefix);
    free(cfg->config_path);
    free(cfg->keys);
    if (cfg->env_values) {
        for (int i = 0; i < cfg->nkeys; i++)
            free(cfg->env_values[i]);
        free(cfg->env_values);
    }
    free(cfg->sorted_keys);
    free(cfg->sections);
    yaml_free(cfg->doc);

    /* Walk and free the per-key DB value cache. */
    if (cfg->db_cache_head) {
        db_val_cache_t *e = *cfg->db_cache_head;
        while (e) {
            db_val_cache_t *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
        free(cfg->db_cache_head);
    }

    if (cfg->mutex) {
        pthread_mutex_destroy(cfg->mutex);
        free(cfg->mutex);
    }

    free(cfg);
}

/* --- DB config integration --- */

/* Find the cache slot for `key`, creating it if missing. Returns NULL on
 * allocation failure. The slot's `value` pointer is stable for the
 * config's lifetime (or until the same slot is refreshed via this
 * helper), so callers can safely hold the returned string across other
 * config_get_* calls on different keys. */
static db_val_cache_t *db_val_cache_slot(const cutils_config_t *cfg, const char *key)
{
    if (!cfg->db_cache_head) return NULL;
    for (db_val_cache_t *e = *cfg->db_cache_head; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;

    db_val_cache_t *slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;
    slot->key = strdup(key);
    if (!slot->key) { free(slot); return NULL; }
    slot->next          = *cfg->db_cache_head;
    *cfg->db_cache_head = slot;
    return slot;
}

/* Invalidate (but don't remove) a cached value — the slot stays so
 * future lookups still hit the same pointer. Called from config_set_db
 * so the next config_get_str refetches. */
static void db_val_cache_invalidate(const cutils_config_t *cfg, const char *key)
{
    if (!cfg->db_cache_head) return;
    for (db_val_cache_t *e = *cfg->db_cache_head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = NULL;
            return;
        }
    }
}

static const char *config_get_from_db(const cutils_config_t *cfg, const char *key)
{
    if (!cfg->db) return NULL;

    const char *params[] = { key, NULL };
    CUTILS_AUTO_DBRES db_result_t *result = NULL;

    int rc = db_execute(cfg->db, "SELECT value FROM config WHERE key = ?",
                        params, &result);
    if (rc != CUTILS_OK || !result || result->nrows == 0)
        return NULL;

    /* Store into a per-key slot so each key has its own stable buffer.
     * The slot is reached through cfg->db_cache_head (heap-allocated),
     * so writing through it does not violate cfg's const-ness. */
    db_val_cache_t *slot = db_val_cache_slot(cfg, key);
    if (!slot) return NULL;

    char *fresh = strdup(result->rows[0][0]);
    if (!fresh) return NULL;
    free(slot->value);
    slot->value = fresh;
    return slot->value;
}

int config_attach_db(cutils_config_t *cfg, cutils_db_t *db,
                     const config_key_t *db_keys)
{
    CUTILS_LOCK_GUARD(cfg->mutex);

    cfg->db = db;

    if (!db_keys) return CUTILS_OK;

    /* Register DB-backed keys. The sorted-key index is rebuilt at the
     * end of this function once all db keys have been appended so
     * lookups after attach see a complete sorted view. */
    for (int i = 0; db_keys[i].key != NULL; i++) {
        if (db_keys[i].store != CFG_STORE_DB) continue;

        int rc = add_key(cfg, &db_keys[i]);
        if (rc != CUTILS_OK) return rc;
    }

    /* Seed defaults into DB for any keys that don't exist yet */
    for (int i = 0; db_keys[i].key != NULL; i++) {
        if (db_keys[i].store != CFG_STORE_DB) continue;

        /* Check if already in DB */
        const char *params[] = { db_keys[i].key, NULL };
        CUTILS_AUTO_DBRES db_result_t *result = NULL;
        int rc = db_execute(db, "SELECT key FROM config WHERE key = ?",
                            params, &result);
        if (rc != CUTILS_OK) return rc;

        if (result->nrows > 0) continue;

        /* Insert with defaults */
        const char *type_str = "string";
        if (db_keys[i].type == CFG_INT) type_str = "int";
        else if (db_keys[i].type == CFG_BOOL) type_str = "bool";

        const char *ins_params[] = {
            db_keys[i].key,
            db_keys[i].default_value ? db_keys[i].default_value : "",
            type_str,
            db_keys[i].default_value ? db_keys[i].default_value : "",
            db_keys[i].description ? db_keys[i].description : "",
            NULL
        };
        rc = db_execute_non_query(db,
            "INSERT INTO config (key, value, type, default_value, description) "
            "VALUES (?, ?, ?, ?, ?)",
            ins_params, NULL);
        if (rc != CUTILS_OK) return rc;
    }

    /* Rebuild the sorted-key index now that all db keys have been
     * appended. Reads after this point use bsearch through the
     * combined file-key + db-key view. */
    return sorted_keys_rebuild(cfg);
}

/* --- Read API --- */

const char *config_get_str(const cutils_config_t *cfg, const char *key)
{
    /* Cast away const on the mutex pointer. Locking is non-mutating from
     * the caller's perspective (a "logically const" config). The lock
     * protects the YAML doc walk, the keys-array walk, and the DB
     * cache mutation inside config_get_from_db. cfg->mutex is recursive
     * so config_get_int / config_get_bool can hold it across this
     * call. */
    CUTILS_LOCK_GUARD(cfg->mutex);

    /* O(log n) lookup via the sorted-by-key index built at the tail of
     * each registration phase. Replaces a linear scan that dominated
     * config_get_str cost for apps with many registered keys. */
    int idx = find_key_idx(cfg, key);

    /* 1. Env var override (captured at registration; see
     * capture_env_for_key). Environment changes after appguard_init are
     * intentionally not observed — this matches the documented
     * "12-factor: set env once at startup" usage and removes a per-read
     * getenv() syscall plus its POSIX setenv hazard. */
    if (idx >= 0 && cfg->env_values[idx])
        return cfg->env_values[idx];

    const config_key_t *def = idx >= 0 ? &cfg->keys[idx] : NULL;

    /* 2. Check the appropriate store */
    if (def && def->store == CFG_STORE_DB) {
        const char *val = config_get_from_db(cfg, key);
        if (val && val[0] != '\0') return val;
    } else {
        if (cfg->doc) {
            const char *val = yaml_get(cfg->doc, key);
            if (val && val[0] != '\0') return val;
        }
    }

    /* 3. Compiled-in default */
    if (def) return def->default_value;

    return NULL;
}

int config_get_int(const cutils_config_t *cfg, const char *key, int default_val)
{
    const char *val = config_get_str(cfg, key);
    if (!val || val[0] == '\0') return default_val;

    char *end;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0') return default_val;
    return (int)v;
}

int config_get_bool(const cutils_config_t *cfg, const char *key, int default_val)
{
    const char *val = config_get_str(cfg, key);
    if (!val || val[0] == '\0') return default_val;

    if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
        strcmp(val, "yes") == 0)
        return 1;
    if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
        strcmp(val, "no") == 0)
        return 0;

    return default_val;
}

/* --- Mutation API --- */

int config_set(cutils_config_t *cfg, const char *key, const char *value)
{
    CUTILS_LOCK_GUARD(cfg->mutex);

    int idx = find_key_idx(cfg, key);
    if (idx < 0)
        return set_error(CUTILS_ERR_NOT_FOUND, "config key '%s' not found", key);
    if (cfg->keys[idx].store != CFG_STORE_FILE)
        return set_error(CUTILS_ERR_INVALID,
            "key '%s' is DB-backed, not file-backed", key);

    if (is_internal_minimum(key))
        return set_error(CUTILS_ERR_INVALID,
            "config key '%s' is a hard minimum and cannot be modified", key);

    /* Update in-memory document */
    if (yaml_set(cfg->doc, key, value) != 0)
        return set_error(CUTILS_ERR_NOT_FOUND,
            "key '%s' not found in config file", key);

    /* Write back to disk */
    if (yaml_write_file(cfg->doc, cfg->config_path) != 0)
        return set_error(CUTILS_ERR_IO, "failed to write config file");

    return CUTILS_OK;
}

/* --- Query API --- */

int config_has_key(const cutils_config_t *cfg, const char *key)
{
    CUTILS_LOCK_GUARD(cfg->mutex);
    return find_key_idx(cfg, key) >= 0;
}

const config_key_t *config_get_key_def(const cutils_config_t *cfg, const char *key)
{
    /* Recursive mutex — config_get_str holds it across the call below. */
    CUTILS_LOCK_GUARD(cfg->mutex);
    int idx = find_key_idx(cfg, key);
    return idx >= 0 ? &cfg->keys[idx] : NULL;
}

int config_file_key_count(const cutils_config_t *cfg)
{
    CUTILS_LOCK_GUARD(cfg->mutex);
    int count = 0;
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].store == CFG_STORE_FILE)
            count++;
    }
    return count;
}

int config_db_key_count(const cutils_config_t *cfg)
{
    CUTILS_LOCK_GUARD(cfg->mutex);
    int count = 0;
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].store == CFG_STORE_DB)
            count++;
    }
    return count;
}

int config_set_db(cutils_config_t *cfg, const char *key, const char *value)
{
    CUTILS_LOCK_GUARD(cfg->mutex);

    if (!cfg->db)
        return set_error(CUTILS_ERR_INVALID, "DB not attached to config");

    /* Verify key exists and is DB-backed */
    const config_key_t *def = config_get_key_def(cfg, key);
    if (!def)
        return set_error(CUTILS_ERR_NOT_FOUND, "config key '%s' not found", key);
    if (def->store != CFG_STORE_DB)
        return set_error(CUTILS_ERR_INVALID,
            "key '%s' is file-backed, use config_set()", key);

    const char *params[] = { value, key, NULL };
    int rc = db_execute_non_query(cfg->db,
        "UPDATE config SET value = ? WHERE key = ?", params, NULL);
    if (rc != CUTILS_OK) return rc;

    /* Drop any cached read so the next config_get_str refetches. */
    db_val_cache_invalidate(cfg, key);
    return CUTILS_OK;
}
