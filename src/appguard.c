#include "cutils/appguard.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"
#include "cutils/mem.h"
#include "cutils/push.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* File-local scoped cleanup so appguard_init can return NULL on any
 * failure step and the guard (with whatever partial state it holds)
 * is shut down correctly. appguard_shutdown is idempotent and only
 * tears down subsystems that have their *_started flag set, so it
 * works on any partially-initialized guard. */
static void appguard_shutdown_p(appguard_t **g)
{
    if (*g) {
        appguard_shutdown(*g);
        *g = NULL;
    }
}
#define CUTILS_AUTO_APPGUARD __attribute__((cleanup(appguard_shutdown_p)))

struct appguard {
    cutils_config_t *config;
    cutils_db_t     *db;
    int              push_started;
    int              log_started;
    int              shutdown_called;
};

/* Saved argv for restart */
static int    saved_argc = 0;
static char **saved_argv = NULL;

/* --- Signal handling --- */

static appguard_t *signal_guard = NULL;

static void signal_handler(int sig)
{
    const char *name = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
    fprintf(stderr, "\nReceived %s, shutting down...\n", name);
    if (signal_guard)
        appguard_shutdown(signal_guard);
    _exit(0);
}

static void install_signal_handlers(appguard_t *guard)
{
    signal_guard = guard;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* --- Init --- */

appguard_t *appguard_init(const appguard_config_t *cfg)
{
    if (!cfg || !cfg->app_name) {
        fprintf(stderr, "appguard_init: app_name is required\n");
        return NULL;
    }

    CUTILS_AUTO_APPGUARD appguard_t *guard = calloc(1, sizeof(*guard));
    if (!guard) {
        fprintf(stderr, "appguard_init: allocation failed\n");
        return NULL;
    }

    /* All `return NULL;` statements below are covered by the cleanup
     * attribute on `guard`. cppcheck does not model __attribute__((cleanup))
     * so suppress its false-positive memleak warnings for this scope. */
    /* cppcheck-suppress-begin memleak */

    /* Step 1-2: Config file (parse or generate + validate) */
    int rc = config_init(&guard->config,
                         cfg->app_name,
                         cfg->config_path,
                         cfg->on_first_run,
                         cfg->file_keys,
                         cfg->sections);

    if (rc == CUTILS_ERR_NOT_FOUND) {
        /* Template generated, first_run = EXIT — silent cleanup. */
        return NULL;
    }
    if (rc != CUTILS_OK) {
        fprintf(stderr, "appguard: config init failed: %s\n", cutils_get_error());
        return NULL;
    }

    /* Step 3: Open DB */
    const char *db_path = config_get_str(guard->config, "db.path");
    rc = db_open(&guard->db, db_path);
    if (rc != CUTILS_OK) {
        fprintf(stderr, "appguard: DB open failed: %s\n", cutils_get_error());
        return NULL;
    }

    /* Step 4: Run lib migrations */
    rc = db_run_lib_migrations(guard->db);
    if (rc != CUTILS_OK) {
        fprintf(stderr, "appguard: lib migrations failed: %s\n",
                cutils_get_error());
        return NULL;
    }

    /* Step 5a: Run compiled-in app migrations */
    if (cfg->migrations) {
        rc = db_run_compiled_migrations(guard->db, cfg->migrations);
        if (rc != CUTILS_OK) {
            fprintf(stderr, "appguard: compiled migrations failed: %s\n",
                    cutils_get_error());
            return NULL;
        }
    }

    /* Step 5b: Run file-based app migrations */
    if (cfg->migrations_dir) {
        rc = db_run_app_migrations(guard->db, cfg->migrations_dir);
        if (rc != CUTILS_OK) {
            fprintf(stderr, "appguard: app migrations failed: %s\n",
                    cutils_get_error());
            return NULL;
        }
    }

    /* Step 6: Attach DB config + seed defaults */
    rc = config_attach_db(guard->config, guard->db, cfg->db_keys);
    if (rc != CUTILS_OK) {
        fprintf(stderr, "appguard: DB config attach failed: %s\n",
                cutils_get_error());
        return NULL;
    }

    /* Step 7: Start log writer */
    log_level_t level = cfg->log_level ? cfg->log_level : LOG_INFO;

    /* Check if log level is overridden in config */
    const char *log_level_str = config_get_str(guard->config, "log.level");
    if (log_level_str && log_level_str[0]) {
        if (strcmp(log_level_str, "debug") == 0)        level = LOG_DEBUG;
        else if (strcmp(log_level_str, "info") == 0)    level = LOG_INFO;
        else if (strcmp(log_level_str, "warning") == 0) level = LOG_WARNING;
        else if (strcmp(log_level_str, "error") == 0)   level = LOG_ERROR;
    }

    rc = log_init(guard->db, level, cfg->log_retention_days);
    if (rc != CUTILS_OK) {
        fprintf(stderr, "appguard: log init failed: %s\n", cutils_get_error());
        return NULL;
    }
    guard->log_started = 1;

    log_info("c-utils initialized for %s", cfg->app_name);

    /* Step 8: Start push worker (if enabled) */
    if (cfg->enable_pushover) {
        rc = push_init(guard->db, guard->config);
        if (rc != CUTILS_OK) {
            log_warn("push init failed: %s (continuing without push)",
                     cutils_get_error());
        } else {
            guard->push_started = 1;
        }
    }

    /* Step 9: Install signal handlers */
    install_signal_handlers(guard);

    /* All steps succeeded — caller takes ownership. */
    return CUTILS_MOVE(guard);
    /* cppcheck-suppress-end memleak */
}

/* --- Shutdown --- */

void appguard_shutdown(appguard_t *guard)
{
    if (!guard || guard->shutdown_called) return;
    guard->shutdown_called = 1;

    log_info("c-utils shutting down");

    /* Reverse order */
    if (guard->push_started)
        push_shutdown();

    if (guard->log_started)
        log_shutdown();

    if (guard->db)
        db_close(guard->db);

    if (guard->config)
        config_free(guard->config);

    signal_guard = NULL;
    free(guard);
}

/* --- Restart --- */

void appguard_set_argv(int argc, char **argv)
{
    saved_argc = argc;
    saved_argv = argv;
}

int appguard_restart(appguard_t *guard)
{
    if (!saved_argv || saved_argc < 1) {
        fprintf(stderr, "appguard_restart: argv not set (call appguard_set_argv first)\n");
        return -1;
    }

    log_info("restarting: %s", saved_argv[0]);

    appguard_shutdown(guard);

    /* Reset signal handlers to default before exec */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    execv(saved_argv[0], saved_argv);

    /* execv only returns on failure */
    fprintf(stderr, "appguard_restart: execv failed: %s\n", strerror(errno));
    return -1;
}

/* --- Accessors --- */

cutils_config_t *appguard_config(appguard_t *guard)
{
    return guard ? guard->config : NULL;
}

cutils_db_t *appguard_db(appguard_t *guard)
{
    return guard ? guard->db : NULL;
}
