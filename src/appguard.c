#include "cutils/appguard.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"
#include "cutils/mem.h"
#include "cutils/push.h"

#include <errno.h>
#include <pthread.h>
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
    pthread_t        signal_thread;
    int              signal_thread_started;
};

/* Saved argv for restart */
static int    saved_argc = 0;
static char **saved_argv = NULL;

/* --- Signal handling ---
 *
 * The pre-fix design used sigaction with a handler that called
 * fprintf, pthread_mutex_lock (via appguard_shutdown → log_info), and
 * other non-async-signal-safe primitives — explicit UB by POSIX, and
 * a deadlock waiting to happen in a multi-threaded program where the
 * signal could land on a thread already holding a c-utils mutex.
 *
 * Replacement design: at appguard_init, block SIGINT/SIGTERM/SIGUSR1
 * process-wide via pthread_sigmask. All subsequently-spawned threads
 * (log writer, push worker, app threads) inherit this mask, so the
 * dedicated signal_watcher thread is the only one that receives the
 * signals. The watcher uses sigwait — synchronous, regular thread
 * context — and can legally call any function. SIGUSR1 is reserved
 * as the "wake the watcher for normal shutdown" sentinel. */

static void appguard_signal_set(sigset_t *out)
{
    sigemptyset(out);
    sigaddset(out, SIGINT);
    sigaddset(out, SIGTERM);
    sigaddset(out, SIGUSR1);
}

static void *signal_watcher(void *arg)
{
    appguard_t *guard = arg;
    sigset_t    set;
    appguard_signal_set(&set);

    int sig = 0;
    if (sigwait(&set, &sig) != 0)
        return NULL;

    if (sig == SIGUSR1) {
        /* appguard_shutdown is driving teardown from the main thread
         * — leave it to do the work and just exit the watcher. */
        return NULL;
    }

    /* SIGINT or SIGTERM: drive the exit ourselves. We're a regular
     * thread, so locking primitives and fprintf are legal. We don't
     * call appguard_shutdown — it would try to join us and self-
     * deadlock. We also skip free() of the guard/config; the kernel
     * reclaims memory on _exit, and an in-flight main-thread reader
     * might still be touching them. We do still drain the subsystem
     * queues so users' last log lines and notifications get through. */
    const char *name = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
    fprintf(stderr, "\nReceived %s, shutting down...\n", name);

    if (guard->push_started) push_shutdown();
    if (guard->log_started)  log_shutdown();
    if (guard->db)           db_close(guard->db);

    /* exit(0) rather than _exit(0): we want any atexit handlers the
     * consuming app registered to fire (resource cleanup, telemetry
     * flush) on signal-driven shutdown — that's the same disposition
     * the app gets on a normal main() return. As a side benefit, the
     * libgcov atexit handler runs too, so coverage data is flushed
     * from the watcher's path under fork-based tests. */
    exit(0);
}

static int install_signal_watcher(appguard_t *guard)
{
    /* Mask was set process-wide at the top of appguard_init; this is
     * purely the watcher-thread spawn step. */
    if (pthread_create(&guard->signal_thread, NULL, signal_watcher, guard) != 0)
        return -1;
    guard->signal_thread_started = 1;
    return 0;
}

/* --- Init --- */

appguard_t *appguard_init(const appguard_config_t *cfg)
{
    if (!cfg || !cfg->app_name) {
        fprintf(stderr, "appguard_init: app_name is required\n");
        return NULL;
    }

    /* Block SIGINT/SIGTERM/SIGUSR1 BEFORE any thread is spawned so the
     * subsequently-created log writer, push worker, and signal_watcher
     * all inherit this mask. The watcher is then the only thread the
     * kernel can deliver these signals to. Done first thing so a
     * signal arriving during init still lands on the watcher (or sits
     * pending until it's spawned in step 9). */
    {
        sigset_t set;
        appguard_signal_set(&set);
        pthread_sigmask(SIG_BLOCK, &set, NULL);
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

    /* Systemd console mode: opted in by the app, gated on whether our
     * stdio is actually wired to journald. JOURNAL_STREAM is set by
     * systemd specifically for this detection (format: "device:inode"
     * of the journal socket). */
    if (cfg->log_systemd_autodetect && getenv("JOURNAL_STREAM") != NULL)
        log_set_systemd_mode(1);

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

    /* Step 9: Spawn signal watcher thread (sigwait-based, see file header). */
    if (install_signal_watcher(guard) != 0) {
        log_warn("signal watcher spawn failed: %s",
                 "shutdown via SIGINT/SIGTERM will not drain subsystems");
        /* Non-fatal: app continues without graceful signal handling. */
    }

    /* All steps succeeded — caller takes ownership. */
    return CUTILS_MOVE(guard);
    /* cppcheck-suppress-end memleak */
}

/* --- Shutdown --- */

void appguard_shutdown(appguard_t *guard)
{
    if (!guard || guard->shutdown_called) return;
    guard->shutdown_called = 1;

    /* Stop the signal watcher BEFORE tearing down subsystems so it
     * doesn't race us on push/log/db cleanup. SIGUSR1 wakes its
     * sigwait; the watcher returns and is joined here. If a real
     * SIGINT/SIGTERM races our SIGUSR1, the watcher takes the exit
     * path and _exits — pthread_join is moot since the process is
     * gone, which is the same outcome the user expected from Ctrl-C. */
    if (guard->signal_thread_started) {
        pthread_kill(guard->signal_thread, SIGUSR1);
        pthread_join(guard->signal_thread, NULL);
        guard->signal_thread_started = 0;
    }

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
        appguard_shutdown(guard);
        return -1;
    }

    log_info("restarting: %s", saved_argv[0]);

    appguard_shutdown(guard);

    /* Restore default signal disposition for the execv'd process. The
     * mask we set in appguard_init is inherited across execve, so
     * unblock here — otherwise the new process starts with SIGINT and
     * SIGTERM blocked until its own appguard_init reblocks them, which
     * would mask any signal arriving in that gap. */
    {
        sigset_t set;
        appguard_signal_set(&set);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    }

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
