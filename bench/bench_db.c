#include "bench.h"
#include "cutils/db.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

/* --- Statement cache (1.7.0) ---
 *
 * Models the per-file catalog work of a backup tool: for each file, 8
 * distinct short statements (4 point lookups, 4 upserts) against
 * indexed tables. The whole run sits in one transaction, so the timed
 * body is prepare + bind + step, not fsync. The _cached and _once
 * variants run identical SQL; the difference is the parse and plan the
 * statement cache saves. */

#define BENCH_CACHE_PATH "/tmp/cutils_bench_db_cache.sqlite"

static const char *const per_file_sql[8] = {
    "SELECT id, size, mtime FROM files WHERE path = ?",
    "SELECT id FROM dirs WHERE path = ?",
    "SELECT refcount FROM blobs WHERE hash = ?",
    "SELECT max(version) FROM versions WHERE file_id = ?",
    "INSERT OR REPLACE INTO files(path, size, mtime) VALUES (?, ?, ?)",
    "INSERT OR REPLACE INTO dirs(path) VALUES (?)",
    "INSERT OR REPLACE INTO blobs(hash, refcount) VALUES (?, 1)",
    "INSERT OR REPLACE INTO versions(file_id, version) VALUES (?, ?)",
};

static void setup_cache_db(cutils_db_t **db_out)
{
    unlink(BENCH_CACHE_PATH);
    cutils_db_t *db = NULL;
    if (db_open(&db, BENCH_CACHE_PATH) != 0 ||
        db_exec_raw(db,
            "CREATE TABLE files(id INTEGER PRIMARY KEY, path TEXT UNIQUE,"
            " size INTEGER, mtime INTEGER);"
            "CREATE TABLE dirs(id INTEGER PRIMARY KEY, path TEXT UNIQUE);"
            "CREATE TABLE blobs(hash BLOB PRIMARY KEY, refcount INTEGER);"
            "CREATE TABLE versions(file_id INTEGER, version INTEGER,"
            " PRIMARY KEY(file_id, version));") != 0 ||
        db_begin(db) != 0) {
        fputs("bench_db: cache fixture failed\n", stderr); exit(1);
    }
    *db_out = db;
}

static void teardown_cache_db(cutils_db_t *db)
{
    if (db_commit(db) != 0) {
        fputs("bench_db: commit failed\n", stderr); exit(1);
    }
    db_close(db);
    unlink(BENCH_CACHE_PATH);
    unlink(BENCH_CACHE_PATH "-wal");
    unlink(BENCH_CACHE_PATH "-shm");
}

static void per_file_run(bench_ctx_t *ctx, int cached)
{
    cutils_db_t *db = NULL;
    setup_cache_db(&db);
    size_t n = 0;

    BENCH_LOOP(ctx) {
        char path[48], dir[32], id[24];
        snprintf(path, sizeof(path), "/data/d%zu/file-%zu", n % 64, n);
        snprintf(dir, sizeof(dir), "/data/d%zu", n % 64);
        snprintf(id, sizeof(id), "%zu", n);
        const char *const args[8][4] = {
            { path, NULL }, { dir, NULL }, { path, NULL }, { id, NULL },
            { path, id, id, NULL }, { dir, NULL }, { path, NULL }, { id, id, NULL },
        };
        for (int s = 0; s < 8; s++) {
            const char **p = (const char **)(uintptr_t)args[s];
            int rc;
            if (s < 4) {
                db_result_t *r = NULL;
                rc = cached ? db_execute(db, per_file_sql[s], p, &r)
                            : db_execute_once(db, per_file_sql[s], p, &r);
                if (rc == 0) BENCH_USE(r->nrows);
                db_result_free(r);
            } else {
                rc = cached ? db_execute_non_query(db, per_file_sql[s], p, NULL)
                            : db_execute_non_query_once(db, per_file_sql[s], p, NULL);
            }
            if (rc != 0) {
                fputs("bench_db: per-file statement failed\n", stderr); exit(1);
            }
        }
        n++;
    }

    teardown_cache_db(db);
}

void bench_db_per_file_8_cached(bench_ctx_t *ctx) { per_file_run(ctx, 1); }
void bench_db_per_file_8_once  (bench_ctx_t *ctx) { per_file_run(ctx, 0); }

/* One c-backup style hash lookup with a 32-byte BLOB key through the
 * typed API, cached. */
void bench_db_typed_blob_lookup(bench_ctx_t *ctx)
{
    cutils_db_t *db = NULL;
    setup_cache_db(&db);
    unsigned char hash[32];
    for (int i = 0; i < 256; i++) {
        memset(hash, i, sizeof(hash));
        db_param_t p[] = { DB_BLOB(hash, sizeof(hash)) };
        if (db_execute_non_query_typed(db, per_file_sql[6], p, 1, NULL) != 0) {
            fputs("bench_db: blob insert failed\n", stderr); exit(1);
        }
    }
    size_t n = 0;

    BENCH_LOOP(ctx) {
        memset(hash, (int)(n++ & 0xff), sizeof(hash));
        db_param_t p[] = { DB_BLOB(hash, sizeof(hash)) };
        db_result_t *r = NULL;
        if (db_execute_typed(db, per_file_sql[2], p, 1, &r) != 0 || r->nrows != 1) {
            fputs("bench_db: blob lookup failed\n", stderr); exit(1);
        }
        BENCH_USE(r->nrows);
        db_result_free(r);
    }

    teardown_cache_db(db);
}
