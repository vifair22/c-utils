#ifndef CUTILS_CONFIG_H
#define CUTILS_CONFIG_H

#include <stddef.h>

#include "cutils/mem.h"

/* --- Config manager ---
 *
 * Two-store config system:
 *   1. YAML file — bootstrap + persistent settings
 *   2. SQLite DB — runtime-mutable app settings
 *
 * Keys use dot-notation ("ups.device"). Each key lives in exactly one store.
 * Read precedence: ENV_VAR > store value > compiled-in default.
 *
 * Quick reference:
 *
 *   // Set the env override BEFORE calling appguard_init / config_init.
 *   // Late setenv after init is NOT observed (env values are
 *   // snapshotted at registration time, see the env-var paragraph
 *   // below for the why).
 *
 *   // Define the keys the app cares about (static array, NULL-sentinel):
 *   static const config_key_t app_keys[] = {
 *       { "sensor.host", CFG_STRING, "127.0.0.1", "Sensor host",
 *         CFG_STORE_FILE, 1 },
 *       { "sensor.port", CFG_INT, "8080", "Sensor port",
 *         CFG_STORE_FILE, 0 },
 *       { NULL, 0, NULL, NULL, 0, 0 }
 *   };
 *
 *   // Reads — same API for file-backed and DB-backed keys.
 *   // appguard_config(guard) gives the cutils_config_t * handle.
 *   const char *host = config_get_str(cfg, "sensor.host");
 *   int port = config_get_int(cfg, "sensor.port", 8080);
 *
 *   // Mutations:
 *   config_set(cfg, "sensor.host", "10.0.0.1");      // file key
 *   config_set_db(cfg, "session.token", new_token);  // DB key
 *
 * The YAML format is controlled by c-utils: simple section/key/value,
 * no flow syntax, no anchors. Comments are preserved on mutation.
 *
 * YAML line-length limit: individual lines in the YAML file MUST be
 * ≤1024 bytes including the trailing newline. The parser reads lines
 * into fixed-size stack buffers; longer lines truncate silently and
 * may produce a partial parse. This is sufficient for any realistic
 * key/value pair (a 1024-byte value would be longer than most URLs
 * or shell-style command strings), but is documented so callers
 * working with abnormally large values know to set them via the DB
 * store rather than the YAML store. Section names and key names are
 * additionally bounded to 128 bytes each. */

/* Value types */
typedef enum {
    CFG_STRING,
    CFG_INT,
    CFG_BOOL,
} config_type_t;

/* Which store owns the key */
typedef enum {
    CFG_STORE_FILE,
    CFG_STORE_DB,
} config_store_t;

/* Key definition — provided as static arrays by the app */
typedef struct {
    const char    *key;           /* dot-notation: "ups.device" */
    config_type_t  type;
    const char    *default_value; /* always a string: "9600", "true", "" */
    const char    *description;   /* shown as YAML comment */
    config_store_t store;
    int            required;      /* 1 = must be non-empty after load */
} config_key_t;

/* Section display name — associates a dot-prefix with a human label */
typedef struct {
    const char *prefix;       /* "ups" */
    const char *display_name; /* "UPS Configuration" */
} config_section_t;

/* First-run behavior when config file doesn't exist */
typedef enum {
    CFG_FIRST_RUN_EXIT,     /* generate template and exit (default) */
    CFG_FIRST_RUN_CONTINUE, /* generate template with defaults and continue */
} config_first_run_t;

/* Opaque config handle */
typedef struct cutils_config cutils_config_t;

/* --- Init / teardown --- */

/* Initialize the config system.
 * Merges c-utils internal keys with app-provided keys and sections.
 * Loads (or generates) the YAML config file.
 * app_name is used for env var prefix (uppercase + underscore).
 * config_path is the YAML file path. When NULL, falls back to the
 * $<APP>_CONFIG_PATH env var if set, then to "config.yaml" in CWD.
 *
 * file_keys and sections may be NULL if the app has no file-backed config.
 * Both arrays must be terminated with a {NULL} sentinel entry.
 *
 * Returns CUTILS_OK, CUTILS_ERR_CONFIG (validation), or negative on error.
 * Returns CUTILS_ERR_NOT_FOUND if file was generated and first_run == EXIT. */
CUTILS_MUST_USE
int config_init(cutils_config_t **cfg,
                const char *app_name,
                const char *config_path,
                config_first_run_t first_run,
                const config_key_t *file_keys,
                const config_section_t *sections);

/* Free the config handle. */
void config_free(cutils_config_t *cfg);

/* --- Read API (file-backed keys) --- */

/* Get a string value. Returns the value or default.
 * Checks env var override first, then file value, then compiled-in default.
 * Returns NULL only if the key doesn't exist.
 *
 * Thread-safety: the read API is safe to call concurrently from any
 * thread. Internally it locks the config handle, so torn reads of the
 * keys array, the YAML doc, and the DB cache are not possible. The
 * returned `const char *` may point into the YAML doc, the DB cache,
 * or the env-var snapshot; another thread calling config_set /
 * config_set_db on the same key invalidates the previous return —
 * copy the value before yielding the lock to user code if you need it
 * to outlive a write.
 *
 * Env var precedence: env-var values are snapshotted once when each
 * key is registered (during config_init for file keys, during
 * config_attach_db for DB keys). Changes to the environment after
 * registration are NOT observed by subsequent reads — set env vars
 * before appguard_init / config_init runs. This matches standard
 * 12-factor usage (set once at startup, never mutate) and eliminates
 * the per-read getenv() syscall plus its POSIX setenv-races-getenv
 * hazard. */
const char *config_get_str(const cutils_config_t *cfg, const char *key);

/* Get an integer value. Returns default_val if key doesn't exist or isn't parseable. */
int config_get_int(const cutils_config_t *cfg, const char *key, int default_val);

/* Get a boolean value. Recognizes "true"/"1"/"yes" and "false"/"0"/"no".
 * Returns default_val if key doesn't exist or isn't parseable. */
int config_get_bool(const cutils_config_t *cfg, const char *key, int default_val);

/* --- Mutation API --- */

/* Update a file-backed key. Rewrites the YAML file preserving comments.
 * Returns CUTILS_ERR_INVALID if the key is a hard minimum (immutable).
 * Returns CUTILS_ERR_NOT_FOUND if the key doesn't exist. */
CUTILS_MUST_USE
int config_set(cutils_config_t *cfg, const char *key, const char *value);

/* Update a DB-backed key.
 * Returns CUTILS_ERR_NOT_FOUND if the key doesn't exist.
 * Returns CUTILS_ERR_INVALID if the key is file-backed. */
CUTILS_MUST_USE
int config_set_db(cutils_config_t *cfg, const char *key, const char *value);

/* --- DB config integration --- */

/* Forward declaration */
typedef struct cutils_db cutils_db_t;

/* Register DB-backed keys and attach a database handle.
 * Seeds any new keys into the config table with their defaults.
 * Must be called after config_init() and db_open() + migrations.
 * db_keys array must be terminated with a {NULL} sentinel. */
CUTILS_MUST_USE
int config_attach_db(cutils_config_t *cfg, cutils_db_t *db,
                     const config_key_t *db_keys);

/* --- Query API --- */

/* Check if a key exists in the config system. */
int config_has_key(const cutils_config_t *cfg, const char *key);

/* Get the key definition for a registered key. Returns NULL if not found. */
const config_key_t *config_get_key_def(const cutils_config_t *cfg, const char *key);

/* Get the number of registered file-backed keys. */
int config_file_key_count(const cutils_config_t *cfg);

/* Get the number of registered DB-backed keys. */
int config_db_key_count(const cutils_config_t *cfg);

#endif
