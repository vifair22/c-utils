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
 *   5. Run app migrations (compiled-in and/or from .sql files)
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
    const char             *config_path;     /* NULL = $<APP>_CONFIG_PATH if set, else "config.yaml" in CWD */
    config_first_run_t      on_first_run;    /* CFG_FIRST_RUN_EXIT (default) or CONTINUE */
    const config_key_t     *file_keys;       /* app file-backed keys, NULL-terminated */
    const config_key_t     *db_keys;         /* app DB-backed keys, NULL-terminated */
    const config_section_t *sections;        /* section display names, NULL-terminated */

    /* Subsystem flags */
    int                     enable_pushover; /* start push worker if creds configured */
    int                     log_retention_days; /* 0 = no cleanup */
    log_level_t             log_level;       /* minimum log level */

    /* When 1, AppGuard checks getenv("JOURNAL_STREAM") at init and, if set,
     * switches the log subsystem's console writer into systemd-native mode
     * (no timestamp, no color, all stdout, <N> priority prefix). DB writer
     * and stream callbacks are unaffected. The same binary running outside
     * systemd retains the standard formatter. */
    int                     log_systemd_autodetect;

    /* App migrations (compiled runs first if both are set) */
    const db_migration_t   *migrations;      /* compiled-in app migrations, NULL = none */
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

/* Clean restart: graceful shutdown then exec's the process with original argv.
 * Call appguard_set_argv() early in main() to enable this.
 * On success, does not return.
 * On failure (argv not set, or execv failed), shuts down the guard and
 * returns -1. The guard pointer is invalid after this call regardless of
 * return value — callers should not dereference it again. */
int appguard_restart(appguard_t *guard);

/* Store argc/argv for restart. Call once from main() before appguard_init(). */
void appguard_set_argv(int argc, char **argv);

/* Get the config handle for reading config values. */
cutils_config_t *appguard_config(appguard_t *guard);

/* Get the DB handle for app queries. */
cutils_db_t *appguard_db(appguard_t *guard);

#endif
