#ifndef CUTILS_CONFIG_H
#define CUTILS_CONFIG_H

#include <stddef.h>

/* --- Config manager ---
 *
 * Two-store config system:
 *   1. YAML file — bootstrap + persistent settings
 *   2. SQLite DB — runtime-mutable app settings (see task #6)
 *
 * Keys use dot-notation ("ups.device"). Each key lives in exactly one store.
 * Read precedence: ENV_VAR > store value > compiled-in default.
 *
 * The YAML format is controlled by c-utils: simple section/key/value,
 * no flow syntax, no anchors. Comments are preserved on mutation. */

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
 * config_path is the YAML file path (NULL = "config.yaml" next to binary).
 *
 * file_keys and sections may be NULL if the app has no file-backed config.
 * Both arrays must be terminated with a {NULL} sentinel entry.
 *
 * Returns CUTILS_OK, CUTILS_ERR_CONFIG (validation), or negative on error.
 * Returns CUTILS_ERR_NOT_FOUND if file was generated and first_run == EXIT. */
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
 * Returns NULL only if the key doesn't exist. */
const char *config_get_str(const cutils_config_t *cfg, const char *key);

/* Get an integer value. Returns default_val if key doesn't exist or isn't parseable. */
int config_get_int(const cutils_config_t *cfg, const char *key, int default_val);

/* Get a boolean value. Recognizes "true"/"1"/"yes" and "false"/"0"/"no".
 * Returns default_val if key doesn't exist or isn't parseable. */
int config_get_bool(const cutils_config_t *cfg, const char *key, int default_val);

/* --- Mutation API (file-backed, non-minimum keys only) --- */

/* Update a file-backed key. Rewrites the YAML file preserving comments.
 * Returns CUTILS_ERR_INVALID if the key is a hard minimum (immutable).
 * Returns CUTILS_ERR_NOT_FOUND if the key doesn't exist. */
int config_set(cutils_config_t *cfg, const char *key, const char *value);

/* --- Query API --- */

/* Check if a key exists in the config system. */
int config_has_key(const cutils_config_t *cfg, const char *key);

/* Get the key definition for a registered key. Returns NULL if not found. */
const config_key_t *config_get_key_def(const cutils_config_t *cfg, const char *key);

/* Get the number of registered file-backed keys. */
int config_file_key_count(const cutils_config_t *cfg);

#endif
