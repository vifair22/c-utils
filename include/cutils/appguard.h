#ifndef CUTILS_APPGUARD_H
#define CUTILS_APPGUARD_H

#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/log.h"

/* --- AppGuard: lifecycle manager ---
 *
 * Orchestrates ordered initialization and graceful shutdown of all
 * c-utils subsystems.
 *
 * Init order:
 *   1. Config file (parse or generate template)
 *   2. Validate required file keys
 *   3. Open DB
 *   4. Run lib migrations (compiled-in)
 *   5. Run app migrations (from .sql files)
 *   6. Attach DB config + seed defaults
 *   7. Start log writer thread
 *   8. Start push worker thread (if enabled)
 *   9. Install signal handlers
 *
 * Shutdown order (reverse):
 *   1. Stop push worker
 *   2. Stop log writer
 *   3. Close DB */

/* Init configuration — the app fills this in and passes to appguard_init() */
typedef struct {
    /* Required */
    const char             *app_name;        /* "airies-ups" — used for env var prefix */

    /* Config */
    const char             *config_path;     /* NULL = "config.yaml" next to binary */
    config_first_run_t      on_first_run;    /* CFG_FIRST_RUN_EXIT (default) or CONTINUE */
    const config_key_t     *file_keys;       /* app file-backed keys, NULL-terminated */
    const config_key_t     *db_keys;         /* app DB-backed keys, NULL-terminated */
    const config_section_t *sections;        /* section display names, NULL-terminated */

    /* Subsystem flags */
    int                     enable_pushover; /* start push worker if creds configured */
    int                     log_retention_days; /* 0 = no cleanup */
    log_level_t             log_level;       /* minimum log level */

    /* App migrations */
    const char             *migrations_dir;  /* path to app .sql migrations, NULL = none */
} appguard_config_t;

/* Opaque handle */
typedef struct appguard appguard_t;

/* Initialize all subsystems in order.
 * Returns NULL on fatal error (config missing, DB failure, etc.).
 * On CFG_FIRST_RUN_EXIT, prints message, cleans up, and returns NULL. */
appguard_t *appguard_init(const appguard_config_t *cfg);

/* Graceful shutdown in reverse order. Safe to call multiple times. */
void appguard_shutdown(appguard_t *guard);

/* Get the config handle for reading config values. */
cutils_config_t *appguard_config(appguard_t *guard);

/* Get the DB handle for app queries. */
cutils_db_t *appguard_db(appguard_t *guard);

#endif
