#include "bench.h"
#include "cutils/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* db_execute reading 1000 rows of 3 string columns. Measures the
 * per-cell strdup cost (3000 strdups per call) plus sqlite step
 * overhead — the call shape any daemon dumping a table will hit.
 * The DB is a temp file so disk caching is realistic; the data
 * is pre-loaded once outside the timed loop. */

#define BENCH_DB_PATH "/tmp/cutils_bench_db.sqlite"
#define BENCH_DB_ROWS 1000

static void setup_db(cutils_db_t **db_out)
{
    unlink(BENCH_DB_PATH);
    cutils_db_t *db = NULL;
    if (db_open(&db, BENCH_DB_PATH) != 0) {
        fputs("bench_db: db_open failed\n", stderr); exit(1);
    }

    if (db_exec_raw(db,
        "CREATE TABLE t(a TEXT, b TEXT, c TEXT);") != 0) {
        fputs("bench_db: create failed\n", stderr); exit(1);
    }

    if (db_begin(db) != 0) {
        fputs("bench_db: begin failed\n", stderr); exit(1);
    }
    for (int i = 0; i < BENCH_DB_ROWS; i++) {
        char a[32], b[32], c[32];
        snprintf(a, sizeof(a), "row-%d-a", i);
        snprintf(b, sizeof(b), "row-%d-b", i);
        snprintf(c, sizeof(c), "row-%d-c", i);
        const char *params[] = { a, b, c, NULL };
        if (db_execute_non_query(db,
            "INSERT INTO t(a, b, c) VALUES (?, ?, ?)",
            params, NULL) != 0) {
            fputs("bench_db: insert failed\n", stderr); exit(1);
        }
    }
    if (db_commit(db) != 0) {
        fputs("bench_db: commit failed\n", stderr); exit(1);
    }

    *db_out = db;
}

void bench_db_execute_1000_rows(bench_ctx_t *ctx)
{
    cutils_db_t *db = NULL;
    setup_db(&db);

    BENCH_LOOP(ctx) {
        db_result_t *r = NULL;
        int rc = db_execute(db, "SELECT a, b, c FROM t", NULL, &r);
        if (rc != 0 || !r) {
            fputs("bench_db: select failed\n", stderr); exit(1);
        }
        BENCH_USE(r->nrows);
        db_result_free(r);
    }

    db_close(db);
    unlink(BENCH_DB_PATH);
    unlink(BENCH_DB_PATH "-wal");
    unlink(BENCH_DB_PATH "-shm");
}

/* Same fixture, same row count, same column shape — but consumed via
 * the streaming iterator. Head-to-head comparison against
 * bench_db_execute_1000_rows isolates the per-cell strdup cost in the
 * materializing path: iteration hands out sqlite's internal column
 * pointers directly, no strdup, no heap row array. Expectation is a
 * substantial speedup and O(1) heap regardless of row count. */
void bench_db_iter_1000_rows(bench_ctx_t *ctx)
{
    cutils_db_t *db = NULL;
    setup_db(&db);

    BENCH_LOOP(ctx) {
        cutils_db_iter_t *it = NULL;
        int rc = db_iter_begin(db, "SELECT a, b, c FROM t", NULL, &it);
        if (rc != 0 || !it) {
            fputs("bench_db: iter_begin failed\n", stderr); exit(1);
        }
        const char **row;
        size_t seen = 0;
        while (db_iter_next(it, &row)) {
            BENCH_USE(row[0]);
            BENCH_USE(row[1]);
            BENCH_USE(row[2]);
            seen++;
        }
        BENCH_USE(seen);
        db_iter_end(it);
    }

    db_close(db);
    unlink(BENCH_DB_PATH);
    unlink(BENCH_DB_PATH "-wal");
    unlink(BENCH_DB_PATH "-shm");
}
