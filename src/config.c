#include "cutils/config.h"
#include "cutils/error.h"
#include "config_yaml.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct cutils_config {
    char           *app_name;      /* for env var prefix */
    char           *env_prefix;    /* uppercase: "AIRIES_UPS" */
    char           *config_path;

    /* Merged key registry (internal + app) */
    config_key_t   *keys;
    int             nkeys;
    int             keys_capacity;

    /* Merged sections */
    config_section_t *sections;
    int               nsections;
    int               sections_capacity;

    /* Parsed YAML document */
    yaml_doc_t     *doc;
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
        cfg->keys_capacity = newcap;
    }

    cfg->keys[cfg->nkeys++] = *key;
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

/* --- Init --- */

int config_init(cutils_config_t **out,
                const char *app_name,
                const char *config_path,
                config_first_run_t first_run,
                const config_key_t *file_keys,
                const config_section_t *sections)
{
    cutils_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return set_error(CUTILS_ERR_NOMEM, "config_init: alloc failed");

    cfg->app_name = strdup(app_name);
    cfg->env_prefix = make_env_prefix(app_name);
    cfg->config_path = strdup(config_path ? config_path : "config.yaml");

    if (!cfg->app_name || !cfg->env_prefix || !cfg->config_path) {
        config_free(cfg);
        return set_error(CUTILS_ERR_NOMEM, "config_init: string alloc failed");
    }

    /* Register internal keys and sections */
    for (int i = 0; cutils_internal_keys[i].key != NULL; i++) {
        int rc = add_key(cfg, &cutils_internal_keys[i]);
        if (rc != CUTILS_OK) { config_free(cfg); return rc; }
    }

    for (int i = 0; cutils_internal_sections[i].prefix != NULL; i++) {
        int rc = add_section(cfg, &cutils_internal_sections[i]);
        if (rc != CUTILS_OK) { config_free(cfg); return rc; }
    }

    /* Register app keys (file-backed only at this stage) */
    if (file_keys) {
        for (int i = 0; file_keys[i].key != NULL; i++) {
            if (file_keys[i].store != CFG_STORE_FILE) continue;
            int rc = add_key(cfg, &file_keys[i]);
            if (rc != CUTILS_OK) { config_free(cfg); return rc; }
        }
    }

    if (sections) {
        for (int i = 0; sections[i].prefix != NULL; i++) {
            int rc = add_section(cfg, &sections[i]);
            if (rc != CUTILS_OK) { config_free(cfg); return rc; }
        }
    }

    /* Check if config file exists */
    FILE *test = fopen(cfg->config_path, "r");
    if (!test) {
        /* Generate template */
        fprintf(stderr, "No config file found at '%s' — generating template.\n",
                cfg->config_path);

        /* Only include file-backed keys in template */
        config_key_t *fkeys = calloc((size_t)cfg->nkeys, sizeof(config_key_t));
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
        free(fkeys);

        if (rc != 0) {
            config_free(cfg);
            return set_error(CUTILS_ERR_IO, "failed to write config template");
        }

        if (first_run == CFG_FIRST_RUN_EXIT) {
            fprintf(stderr, "Config template written to '%s'. "
                    "Please review and restart.\n", cfg->config_path);
            config_free(cfg);
            *out = NULL;
            return CUTILS_ERR_NOT_FOUND;
        }

        /* CFG_FIRST_RUN_CONTINUE: fall through to parse the generated file */
    } else {
        fclose(test);
    }

    /* Parse the YAML file */
    cfg->doc = yaml_parse_file(cfg->config_path);
    if (!cfg->doc) {
        config_free(cfg);
        return set_error(CUTILS_ERR_IO, "failed to parse config file '%s'",
                         cfg->config_path);
    }

    /* Validate required keys */
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].store != CFG_STORE_FILE) continue;
        if (!cfg->keys[i].required) continue;

        const char *val = yaml_get(cfg->doc, cfg->keys[i].key);
        if (!val || val[0] == '\0') {
            const char *key = cfg->keys[i].key;
            config_free(cfg);
            return set_error(CUTILS_ERR_CONFIG,
                "required config key '%s' is missing or empty", key);
        }
    }

    *out = cfg;
    return CUTILS_OK;
}

void config_free(cutils_config_t *cfg)
{
    if (!cfg) return;
    free(cfg->app_name);
    free(cfg->env_prefix);
    free(cfg->config_path);
    free(cfg->keys);
    free(cfg->sections);
    yaml_free(cfg->doc);
    free(cfg);
}

/* --- Read API --- */

const char *config_get_str(const cutils_config_t *cfg, const char *key)
{
    /* 1. Check env var override */
    char *envname = make_env_name(cfg->env_prefix, key);
    if (envname) {
        const char *envval = getenv(envname);
        free(envname);
        if (envval) return envval;
    }

    /* 2. Check YAML file */
    if (cfg->doc) {
        const char *val = yaml_get(cfg->doc, key);
        if (val && val[0] != '\0') return val;
    }

    /* 3. Compiled-in default */
    for (int i = 0; i < cfg->nkeys; i++) {
        if (strcmp(cfg->keys[i].key, key) == 0)
            return cfg->keys[i].default_value;
    }

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
    /* Check key exists */
    int found = 0;
    for (int i = 0; i < cfg->nkeys; i++) {
        if (strcmp(cfg->keys[i].key, key) == 0) {
            if (cfg->keys[i].store != CFG_STORE_FILE)
                return set_error(CUTILS_ERR_INVALID,
                    "key '%s' is DB-backed, not file-backed", key);
            found = 1;
            break;
        }
    }

    if (!found)
        return set_error(CUTILS_ERR_NOT_FOUND, "config key '%s' not found", key);

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
    for (int i = 0; i < cfg->nkeys; i++) {
        if (strcmp(cfg->keys[i].key, key) == 0)
            return 1;
    }
    return 0;
}

const config_key_t *config_get_key_def(const cutils_config_t *cfg, const char *key)
{
    for (int i = 0; i < cfg->nkeys; i++) {
        if (strcmp(cfg->keys[i].key, key) == 0)
            return &cfg->keys[i];
    }
    return NULL;
}

int config_file_key_count(const cutils_config_t *cfg)
{
    int count = 0;
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].store == CFG_STORE_FILE)
            count++;
    }
    return count;
}
