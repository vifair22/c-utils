#ifndef CUTILS_APPGUARD_H
#define CUTILS_APPGUARD_H

#include <sys/types.h>           /* mode_t */

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

    /* --- File-permission enforcement (1.2.0) ---
     *
     * When nonzero, AppGuard chmod's the named artifact(s) to this
     * mode during init. When 0, file permissions are left at whatever
     * umask / pre-existing perms produced (current 1.1.0 behavior).
     *
     * Typical use for a daemon holding sensitive data:
     *   .db_mode     = 0600,
     *   .config_mode = 0600,
     *
     * Failure to chmod is a hard error during init — appguard_init
     * returns NULL and the partially-built guard is torn down. The
     * rationale: opting in to a mode value is a contract assertion
     * ("the file MUST be at this mode"); falling back to a more
     * permissive mode silently would undercut that contract. */

    /* When nonzero, the DB file and its sqlite sidecars (.db-wal,
     * .db-shm) are chmod'd to this mode. To make sqlite create the
     * sidecars with restrictive perms in the first place (avoiding a
     * microsecond race window during init), AppGuard temporarily sets
     * the process umask to ~db_mode & 0777 around the DB-open and
     * migration phases and restores it before init returns. The umask
     * change is localized — it does NOT bleed into the running
     * application.
     *
     * Mid-session edge case: if an external process deletes a sidecar
     * file while the daemon is running and sqlite recreates it, the
     * new file inherits the application's then-current umask, not
     * db_mode. Daemons that need protection against this rare case
     * should set umask(0077) themselves at startup. */
    mode_t                  db_mode;

    /* When nonzero, the YAML config file is chmod'd to this mode
     * after config_init successfully parses it (and after first-run
     * template generation, when applicable). When 0, the file's
     * mode is left at whatever umask / pre-existing perms produced —
     * c-utils does not nag about permissive config files, because
     * leaving it alone is a legitimate choice the application
     * author has expressed by not setting this field. */
    mode_t                  config_mode;

    /* --- Application-driven shutdown (1.3.0) ---
     *
     * By default the first SIGINT or SIGTERM is handled entirely inside
     * AppGuard: the signal watcher drains push and log, then calls
     * exit(0). The application never hears about it, which is fine for a
     * tool and wrong for a daemon with worker threads in flight: they are
     * stopped mid-operation, and the app's own teardown (stop accepting
     * requests, cancel jobs, remove a socket) never runs. Since 1.7.2 the
     * watcher leaves the DB open rather than closing it under those
     * threads; committed transactions are already durable.
     *
     * When on_signal is set, the first SIGINT or SIGTERM instead calls
     * on_signal(sig, on_signal_ctx) from the watcher thread and AppGuard
     * keeps running. The app is expected to wind down and call
     * appguard_shutdown() itself, in its own order. The callback runs on a
     * regular thread, so it may lock, log, or signal a condition variable,
     * but it should return promptly: setting a flag the main loop watches
     * is the intended use.
     *
     * A second SIGINT or SIGTERM takes the default path and exits, so an
     * operator can still stop a process whose shutdown has wedged. NULL
     * keeps the 1.2.0 behavior. */
    void                  (*on_signal)(int sig, void *ctx);
    void                   *on_signal_ctx;

    /* --- Config validation (1.4.0) ---
     *
     * What to do with a config file that has lines outside the supported
     * YAML subset, unknown or misspelled keys, DB-backed keys written in
     * the file, values of the wrong type, or a log.level that is not one
     * of debug, info, warn, warning, error. See config_validate_t.
     *
     * CFG_VALIDATE_OFF (0, the default) keeps 1.3 behavior: all of that
     * loads silently. CFG_VALIDATE_WARN prints each problem to stderr as
     * "<path>:<line>: warning: ..." and continues. CFG_VALIDATE_STRICT
     * prints them as errors and appguard_init returns NULL before the
     * database is opened, which is what a daemon whose config is written
     * by hand wants: a setting that is accepted and ignored is worse
     * than a refusal to start. */
    config_validate_t       config_validate;

    /* Application checks (1.5.0): ranges, enumerations, anything the
     * types do not say. Called during config init with the file loaded,
     * and its problems reported in the same list as the built-in ones.
     * Ignored when config_validate is CFG_VALIDATE_OFF. See
     * config_check_fn in config.h. */
    config_check_fn         config_check;
    void                   *config_check_ctx;
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
