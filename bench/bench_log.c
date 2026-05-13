#include "bench.h"
#include "cutils/log.h"
#include "cutils/db.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* log_write measured in two shapes:
 *
 * - bench_log_write_db_off: log_init(db=NULL, LOG_ERROR retention=0).
 *   Setting the level above the messages we emit so we exercise the
 *   filter short-circuit (the realistic call shape — most log_debug /
 *   log_info calls in a daemon are below the configured threshold).
 *
 * - bench_log_write_db_on: log_init with a real DB. Now the work
 *   path is: vsnprintf the message, format the console line, enqueue
 *   the row for the async DB writer. The DB writer thread drains
 *   concurrently but does NOT block enqueuers — the bench measures
 *   the producer side only.
 *
 * Both flavors redirect stdout to /dev/null for the duration of the
 * BENCH_LOOP so the bench output isn't polluted by thousands of log
 * lines. The original stdout fd is restored after the loop. */

#define BENCH_LOG_DB_PATH "/tmp/cutils_bench_log.sqlite"

static int redirect_stdout_to_devnull(void)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (saved < 0 || devnull < 0) { fputs("bench_log: redirect failed\n", stderr); exit(1); }
    dup2(devnull, STDOUT_FILENO);
    close(devnull);
    return saved;
}

static void restore_stdout(int saved)
{
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
}

void bench_log_write_db_off(bench_ctx_t *ctx)
{
    /* Filter at LOG_ERROR so the log_info calls below short-circuit
     * inside log_write (atomic_load(&log_min_level) compare-and-return).
     * This is the realistic measurement: the dominant production cost
     * is the filter check on debug/info that gets dropped. */
    if (log_init(NULL, LOG_ERROR, 0) != 0) {
        fputs("bench_log: init failed\n", stderr); exit(1);
    }

    int saved = redirect_stdout_to_devnull();

    BENCH_LOOP(ctx) {
        log_info("bench %zu", _bench_i);
    }

    restore_stdout(saved);
    log_shutdown();
}

void bench_log_write_db_on(bench_ctx_t *ctx)
{
    unlink(BENCH_LOG_DB_PATH);
    cutils_db_t *db = NULL;
    if (db_open(&db, BENCH_LOG_DB_PATH) != 0) {
        fputs("bench_log: db_open failed\n", stderr); exit(1);
    }
    if (db_run_lib_migrations(db) != 0) {
        fputs("bench_log: migrations failed\n", stderr); exit(1);
    }

    /* Filter at LOG_INFO so log_info actually fires through the full
     * write path: vsnprintf, console format, async enqueue. The DB
     * writer thread drains concurrently. */
    if (log_init(db, LOG_INFO, 0) != 0) {
        fputs("bench_log: init failed\n", stderr); exit(1);
    }

    int saved = redirect_stdout_to_devnull();

    BENCH_LOOP(ctx) {
        log_info("bench %zu", _bench_i);
    }

    restore_stdout(saved);
    log_shutdown();
    db_close(db);
    unlink(BENCH_LOG_DB_PATH);
    unlink(BENCH_LOG_DB_PATH "-wal");
    unlink(BENCH_LOG_DB_PATH "-shm");
}
