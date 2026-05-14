#include "cutils/appguard.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"
#include "cutils/mem.h"
#include "cutils/push.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

/* Tear down the three background subsystems (push, log, db) in
 * reverse-of-init order, gated on the per-subsystem started flags.
 * Shared between the signal watcher and appguard_shutdown so the two
 * paths can't drift on which subsystems to touch or in what order.
 *
 * Does NOT touch guard->config or free(guard) — those operations
 * differ between callers (the signal watcher exits the process and
 * lets the kernel reclaim; appguard_shutdown frees explicitly). */
static void shutdown_subsystems(appguard_t *guard)
{
    if (guard->push_started) push_shutdown();
    if (guard->log_started)  log_shutdown();
    if (guard->db)           db_close(guard->db);
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

    shutdown_subsystems(guard);

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

/* --- File-permission enforcement helpers (1.2.0) ---
 *
 * db_mode and config_mode in appguard_config_t are opt-in: 0 means
 * leave file perms at umask-derived defaults (1.1.0 behavior), nonzero
 * means chmod the artifact to that mode. Failure is a hard error so
 * the caller's mode-assertion contract is never silently downgraded.
 *
 * For db_mode, we additionally set umask(~db_mode & 0777) around the
 * DB-open + migration phase so sqlite creates .db-wal / .db-shm with
 * the right perms in the first place, eliminating the race window
 * between file creation and our post-hoc chmod. The umask is restored
 * before any application code runs (i.e., before appguard_init
 * returns), so the change does not bleed into the daemon's runtime. */

/* chmod a sidecar file (.db-wal, .db-shm). ENOENT is tolerated — the
 * sidecar may legitimately not exist (e.g., WAL not yet materialized
 * because no writes have happened). Any other error is a hard failure
 * with set_error_errno already invoked.
 *
 * The error path is LCOV_EXCL'd: chmod on a file we own and whose
 * existence we just verified shouldn't fail; exercising the non-ENOENT
 * branch needs an EROFS mount, a permissions-stripping LD_PRELOAD, or
 * similar deliberate harness. Same pattern db_open_with_mode uses. */
static int chmod_sidecar(const char *path, mode_t mode)
{
    if (chmod(path, mode) == 0) return CUTILS_OK;
    if (errno == ENOENT) return CUTILS_OK;
    /* LCOV_EXCL_START */
    return set_error_errno(CUTILS_ERR_IO, "chmod %s", path);
    /* LCOV_EXCL_STOP */
}

/* Build "<db_path><suffix>" into out (size out_sz). Returns 0 on
 * success, -1 on overflow (db_path + suffix wouldn't fit). The
 * overflow branch is LCOV_EXCL'd — exercising it requires a db_path
 * within 5 bytes of PATH_MAX, which is not a realistic test config. */
static int build_sidecar_path(char *out, size_t out_sz,
                              const char *db_path, const char *suffix)
{
    int n = snprintf(out, out_sz, "%s%s", db_path, suffix);
    if (n < 0 || (size_t)n >= out_sz) return -1; /* LCOV_EXCL_LINE */
    return 0;
}

/* Chmod the DB main file and its sqlite WAL/SHM sidecars to `mode`.
 * The main file is treated as required (must exist by this point —
 * db_open succeeded). Sidecars are ENOENT-tolerant. Called after
 * migrations run, so any sidecars sqlite needed should exist. */
static int chmod_db_artifacts(const char *db_path, mode_t mode)
{
    if (chmod(db_path, mode) != 0)
        return set_error_errno(CUTILS_ERR_IO, "chmod %s", db_path); /* LCOV_EXCL_LINE */

    char sidecar[PATH_MAX];
    /* LCOV_EXCL_START — overflow branches are unreachable with realistic db paths. */
    if (build_sidecar_path(sidecar, sizeof(sidecar), db_path, "-wal") != 0)
        return set_error(CUTILS_ERR_IO, "db path too long for -wal sidecar");
    /* LCOV_EXCL_STOP */
    int rc = chmod_sidecar(sidecar, mode);
    if (rc != CUTILS_OK) return rc; /* LCOV_EXCL_LINE */

    /* LCOV_EXCL_START — same as above for the -shm sidecar. */
    if (build_sidecar_path(sidecar, sizeof(sidecar), db_path, "-shm") != 0)
        return set_error(CUTILS_ERR_IO, "db path too long for -shm sidecar");
    /* LCOV_EXCL_STOP */
    return chmod_sidecar(sidecar, mode);
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

    /* Lock down the config file if requested. config_init has
     * already produced the file (parsed or generated), so the path
     * is real and chmod is well-defined. Hard failure: the caller
     * asserted a mode contract by setting config_mode != 0, and we
     * don't silently downgrade. */
    if (cfg->config_mode != 0) {
        const char *path = config_get_path(guard->config);
        if (!path || chmod(path, cfg->config_mode) != 0) {
            /* LCOV_EXCL_START — config_get_path returns non-NULL when
             * config_init succeeded, and chmod on a just-parsed file
             * we read from shouldn't fail. */
            fprintf(stderr, "appguard: chmod config (%s) to 0%o failed: %s\n",
                    path ? path : "(null)", (unsigned)cfg->config_mode,
                    strerror(errno));
            return NULL;
            /* LCOV_EXCL_STOP */
        }
    }

    /* Step 3 prep: when db_mode is set, narrow the process umask so
     * sqlite's lazy .db-wal / .db-shm creates inherit restrictive
     * perms. The umask is restored before init returns (search for
     * "restore umask"). save_umask is initialized but only read on
     * the db_mode != 0 path. */
    mode_t save_umask = 0;
    if (cfg->db_mode != 0)
        save_umask = umask((mode_t)(~cfg->db_mode & 0777));

    /* Step 3: Open DB */
    const char *db_path = config_get_str(guard->config, "db.path");
    rc = (cfg->db_mode != 0)
            ? db_open_with_mode(&guard->db, db_path, cfg->db_mode)
            : db_open(&guard->db, db_path);
    if (rc != CUTILS_OK) {
        if (cfg->db_mode != 0) umask(save_umask);
        fprintf(stderr, "appguard: DB open failed: %s\n", cutils_get_error());
        return NULL;
    }

    /* Step 4: Run lib migrations */
    rc = db_run_lib_migrations(guard->db);
    if (rc != CUTILS_OK) {
        if (cfg->db_mode != 0) umask(save_umask);
        fprintf(stderr, "appguard: lib migrations failed: %s\n",
                cutils_get_error());
        return NULL;
    }

    /* Step 5a: Run compiled-in app migrations */
    if (cfg->migrations) {
        rc = db_run_compiled_migrations(guard->db, cfg->migrations);
        if (rc != CUTILS_OK) {
            if (cfg->db_mode != 0) umask(save_umask);
            fprintf(stderr, "appguard: compiled migrations failed: %s\n",
                    cutils_get_error());
            return NULL;
        }
    }

    /* Step 5b: Run file-based app migrations */
    if (cfg->migrations_dir) {
        rc = db_run_app_migrations(guard->db, cfg->migrations_dir);
        if (rc != CUTILS_OK) {
            if (cfg->db_mode != 0) umask(save_umask);
            fprintf(stderr, "appguard: app migrations failed: %s\n",
                    cutils_get_error());
            return NULL;
        }
    }

    /* Post-migration perm pass: any .db-wal / .db-shm sqlite created
     * during open or migrations are now subject to the restrictive
     * umask, but we also chmod explicitly to cover the
     * pre-existing-file case (a daemon restarting against an old
     * sidecar that was created under a permissive umask). The main
     * .db file was already chmod'd by db_open_with_mode; redoing it
     * here is harmless and keeps all three artifacts in one call.
     * Then restore the umask so subsequent files the app creates
     * are not affected. */
    if (cfg->db_mode != 0) {
        rc = chmod_db_artifacts(db_path, cfg->db_mode);
        umask(save_umask);
        if (rc != CUTILS_OK) {
            /* LCOV_EXCL_START — same unreachability as
             * db_open_with_mode's chmod-failure path. */
            fprintf(stderr, "appguard: chmod DB artifacts to 0%o failed: %s\n",
                    (unsigned)cfg->db_mode, cutils_get_error());
            return NULL;
            /* LCOV_EXCL_STOP */
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

    shutdown_subsystems(guard);

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
