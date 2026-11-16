/* Prepared-statement cache, typed parameters and NULL detection in
 * results (db.c, 1.7.0). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB "/tmp/cutils_test_db_cache.db"

static int setup(void **state)
{
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT UNIQUE)"),
        CUTILS_OK);
    *state = db;
    return 0;
}

static int teardown(void **state)
{
    db_close(*state);
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static db_stmt_cache_stats_t stats(cutils_db_t *db)
{
    db_stmt_cache_stats_t s;
    db_stmt_cache_stats(db, &s);
    return s;
}

static void insert(cutils_db_t *db, const char *name)
{
    const char *p[] = { name, NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t (name) VALUES (?)", p, NULL), CUTILS_OK);
}

static int count_rows(cutils_db_t *db)
{
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT count(*) FROM t", NULL, &r),
                     CUTILS_OK);
    return atoi(r->rows[0][0]);
}

/* --- Cache behaviour --- */

static void test_default_capacity(void **state)
{
    db_stmt_cache_stats_t s = stats(*state);
    assert_int_equal(s.capacity, CUTILS_DB_STMT_CACHE_DEFAULT);
    assert_int_equal(s.entries, 0);
}

static void test_repeat_call_hits(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "a");
    insert(db, "b");
    insert(db, "c");

    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.misses, 1);
    assert_int_equal(s.hits, 2);
    assert_int_equal(s.entries, 1);
    assert_int_equal(count_rows(db), 3);
}

static void test_once_bypasses_cache(void **state)
{
    cutils_db_t *db = *state;
    const char *p[] = { "x", NULL };
    int affected = 0;
    assert_int_equal(db_execute_non_query_once(db,
        "INSERT INTO t (name) VALUES (?)", p, &affected), CUTILS_OK);
    assert_int_equal(affected, 1);

    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute_once(db, "SELECT name FROM t", NULL, &r),
                     CUTILS_OK);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0], "x");

    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.entries, 0);
    assert_int_equal(s.hits + s.misses, 0);
}

/* A reused statement keeps its bindings (no sqlite3_clear_bindings),
 * so a call that passes fewer params than the last one must still see
 * the omitted ones as NULL. */
static void test_omitted_params_read_null(void **state)
{
    cutils_db_t *db = *state;
    const char *sql = "SELECT ?1 IS NULL, ?2 IS NULL, ?3 IS NULL";

    CUTILS_AUTO_DBRES db_result_t *r1 = NULL;
    assert_int_equal(db_execute(db, sql,
        (const char *[]){ "a", "b", "c", NULL }, &r1), CUTILS_OK);
    assert_string_equal(r1->rows[0][0], "0");
    assert_string_equal(r1->rows[0][2], "0");

    CUTILS_AUTO_DBRES db_result_t *r2 = NULL;
    assert_int_equal(db_execute(db, sql,
        (const char *[]){ "a", NULL }, &r2), CUTILS_OK);
    assert_string_equal(r2->rows[0][0], "0");
    assert_string_equal(r2->rows[0][1], "1");
    assert_string_equal(r2->rows[0][2], "1");

    CUTILS_AUTO_DBRES db_result_t *r3 = NULL;
    assert_int_equal(db_execute(db, sql, NULL, &r3), CUTILS_OK);
    assert_string_equal(r3->rows[0][0], "1");

    CUTILS_AUTO_DBRES db_result_t *r4 = NULL;
    assert_int_equal(db_execute(db, sql,
        (const char *[]){ "a", "b", NULL }, &r4), CUTILS_OK);
    assert_string_equal(r4->rows[0][1], "0");
    assert_string_equal(r4->rows[0][2], "1");

    assert_int_equal(stats(db).hits, 3);
}

/* A bound value is held across calls; the next call's own value wins. */
static void test_rebind_replaces_value(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "one");
    insert(db, "two");
    const char *sql = "SELECT id FROM t WHERE name = ?";

    for (int i = 0; i < 3; i++) {
        CUTILS_AUTO_DBRES db_result_t *r = NULL;
        const char *want = (i % 2) ? "two" : "one";
        assert_int_equal(db_execute(db, sql,
            (const char *[]){ want, NULL }, &r), CUTILS_OK);
        assert_int_equal(r->nrows, 1);
        assert_string_equal(r->rows[0][0], (i % 2) ? "2" : "1");
    }
}

static void test_lru_eviction(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(db_stmt_cache_set_size(db, 2), CUTILS_OK);

    const char *a = "SELECT 'a'", *b = "SELECT 'b'", *c = "SELECT 'c'";
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, a, NULL, &r), CUTILS_OK); db_result_free(r);
    assert_int_equal(db_execute(db, b, NULL, &r), CUTILS_OK); db_result_free(r);
    assert_int_equal(db_execute(db, a, NULL, &r), CUTILS_OK); db_result_free(r);
    /* b is now least recently used, so c evicts it. */
    assert_int_equal(db_execute(db, c, NULL, &r), CUTILS_OK); db_result_free(r);

    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.entries, 2);
    assert_int_equal(s.evictions, 1);
    assert_int_equal(s.hits, 1);

    assert_int_equal(db_execute(db, a, NULL, &r), CUTILS_OK); db_result_free(r);
    assert_int_equal(stats(db).hits, 2);
    assert_int_equal(db_execute(db, b, NULL, &r), CUTILS_OK); db_result_free(r);
    s = stats(db);
    assert_int_equal(s.hits, 2);
    assert_int_equal(s.misses, 4);
}

static void test_set_size_bounds_and_disable(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(db_stmt_cache_set_size(db, -1), CUTILS_ERR_INVALID);
    assert_int_equal(db_stmt_cache_set_size(db, CUTILS_DB_STMT_CACHE_MAX + 1),
                     CUTILS_ERR_INVALID);
    assert_int_equal(stats(db).capacity, CUTILS_DB_STMT_CACHE_DEFAULT);

    insert(db, "a");
    insert(db, "b");
    assert_int_equal(stats(db).entries, 1);

    /* Shrinking to zero finalizes everything and turns caching off. */
    assert_int_equal(db_stmt_cache_set_size(db, 0), CUTILS_OK);
    assert_int_equal(stats(db).entries, 0);
    insert(db, "c");
    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.entries, 0);
    assert_int_equal(s.hits, 1);
    assert_int_equal(s.capacity, 0);

    /* And back on, past the initial bucket count to force a rehash. */
    assert_int_equal(db_stmt_cache_set_size(db, CUTILS_DB_STMT_CACHE_MAX),
                     CUTILS_OK);
    insert(db, "d");
    assert_int_equal(stats(db).entries, 1);
    assert_int_equal(count_rows(db), 4);
}

static void test_shrink_keeps_most_recent(void **state)
{
    cutils_db_t *db = *state;
    char sql[64];
    for (int i = 0; i < 10; i++) {
        db_result_t *r = NULL;
        snprintf(sql, sizeof(sql), "SELECT %d", i);
        assert_int_equal(db_execute(db, sql, NULL, &r), CUTILS_OK);
        db_result_free(r);
    }
    assert_int_equal(stats(db).entries, 10);
    assert_int_equal(db_stmt_cache_set_size(db, 3), CUTILS_OK);
    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.entries, 3);
    assert_int_equal(s.evictions, 7);

    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT 9", NULL, &r), CUTILS_OK);
    db_result_free(r);
    assert_int_equal(stats(db).hits, 1);
    assert_int_equal(db_execute(db, "SELECT 0", NULL, &r), CUTILS_OK);
    db_result_free(r);
    assert_int_equal(stats(db).hits, 1);
}

/* Many distinct statements with a small cache: hash chains and the LRU
 * list must stay consistent through constant eviction. */
static void test_churn(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(db_stmt_cache_set_size(db, 5), CUTILS_OK);
    char sql[64];
    for (int i = 0; i < 500; i++) {
        db_result_t *r = NULL;
        int k = (i * 7) % 13;
        snprintf(sql, sizeof(sql), "SELECT %d", k);
        assert_int_equal(db_execute(db, sql, NULL, &r), CUTILS_OK);
        assert_int_equal(atoi(r->rows[0][0]), k);
        db_result_free(r);
    }
    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.entries, 5);
    assert_int_equal(s.hits + s.misses, 500);
}

/* --- Schema changes and failures --- */

static void test_schema_change_reprepares(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "a");
    const char *sql = "SELECT * FROM t";

    CUTILS_AUTO_DBRES db_result_t *r1 = NULL;
    assert_int_equal(db_execute(db, sql, NULL, &r1), CUTILS_OK);
    assert_int_equal(r1->ncols, 2);

    assert_int_equal(db_exec_raw(db,
        "ALTER TABLE t ADD COLUMN extra TEXT DEFAULT 'e'"), CUTILS_OK);

    CUTILS_AUTO_DBRES db_result_t *r2 = NULL;
    assert_int_equal(db_execute(db, sql, NULL, &r2), CUTILS_OK);
    assert_int_equal(r2->ncols, 3);
    assert_string_equal(r2->col_names[2], "extra");
    assert_string_equal(r2->rows[0][2], "e");
    assert_int_equal(stats(db).hits, 1);
}

static void test_dropped_table_fails_then_recovers(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "a");
    int before = stats(db).entries;

    assert_int_equal(db_exec_raw(db, "DROP TABLE t"), CUTILS_OK);
    const char *p[] = { "b", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t (name) VALUES (?)", p, NULL), CUTILS_ERR_DB);
    assert_non_null(strstr(cutils_get_error(), "no such table"));
    assert_int_equal(stats(db).entries, before - 1);

    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT UNIQUE)"),
        CUTILS_OK);
    insert(db, "b");
    assert_int_equal(count_rows(db), 1);
}

static void test_constraint_error_drops_entry(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "dup");
    const char *p[] = { "dup", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t (name) VALUES (?)", p, NULL), CUTILS_ERR_DB);
    assert_non_null(strstr(cutils_get_error(), "UNIQUE"));
    assert_int_equal(stats(db).entries, 0);

    insert(db, "next");
    assert_int_equal(count_rows(db), 2);
}

static void test_bind_error_drops_entry(void **state)
{
    cutils_db_t *db = *state;
    const char *sql = "SELECT ?";
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, sql, (const char *[]){ "a", NULL }, &r),
                     CUTILS_OK);
    db_result_free(r);
    assert_int_equal(db_execute(db, sql,
        (const char *[]){ "a", "b", NULL }, &r), CUTILS_ERR_DB);
    assert_null(r);
    assert_int_equal(stats(db).entries, 0);
}

static void test_prepare_error_not_cached(void **state)
{
    cutils_db_t *db = *state;
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELEKT 1", NULL, &r), CUTILS_ERR_DB);
    assert_non_null(strstr(cutils_get_error(), "prepare"));
    assert_int_equal(stats(db).entries, 0);
}

static void test_once_prepare_error(void **state)
{
    cutils_db_t *db = *state;
    db_result_t *r = NULL;
    assert_int_equal(db_execute_once(db, "SELEKT 1", NULL, &r), CUTILS_ERR_DB);
    assert_null(r);
    assert_non_null(strstr(cutils_get_error(), "prepare"));
}

/* A step error after some rows were read: the partial rows are freed,
 * the entry is dropped and the next call prepares afresh. */
static void test_error_mid_result(void **state)
{
    cutils_db_t *db = *state;
    const char *sql =
        "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n "
        "WHERE i < 40) SELECT CASE WHEN i < 30 THEN i "
        "ELSE abs(-9223372036854775807 - 1) END FROM n";
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, sql, NULL, &r), CUTILS_ERR_DB);
    assert_null(r);
    assert_non_null(strstr(cutils_get_error(), "step"));
    assert_int_equal(stats(db).entries, 0);
    assert_int_equal(db_execute(db, sql, NULL, &r), CUTILS_ERR_DB);
    assert_int_equal(stats(db).misses, 2);
}

static void test_non_query_rejects_rows(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(db_execute_non_query(db, "SELECT 1", NULL, NULL),
                     CUTILS_ERR_DB);
    assert_int_equal(stats(db).entries, 0);
}

/* --- Cached statements must not hold locks between calls --- */

static void test_reset_releases_read_lock(void **state)
{
    cutils_db_t *db = *state;
    for (int i = 0; i < 20; i++) {
        char n[16];
        snprintf(n, sizeof(n), "r%d", i);
        insert(db, n);
    }
    /* A multi-row SELECT that stopped mid-step would keep its read
     * transaction; both of these need none to be open. */
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM t", NULL, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 20);

    CUTILS_AUTO_DBRES db_result_t *ck = NULL;
    assert_int_equal(db_execute_once(db, "PRAGMA wal_checkpoint(TRUNCATE)",
                                     NULL, &ck), CUTILS_OK);
    assert_string_equal(ck->rows[0][0], "0");
    assert_int_equal(db_exec_raw(db, "VACUUM"), CUTILS_OK);
}

static void test_rollback_with_cached_statements(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "kept");
    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        insert(db, "gone");
        assert_int_equal(count_rows(db), 2);
    }
    assert_int_equal(count_rows(db), 1);
    insert(db, "after");
    assert_int_equal(count_rows(db), 2);
}

/* --- Threads --- */

#define N_THREADS 8
#define N_ITERS   300

static void *hammer(void *arg)
{
    cutils_db_t *db = arg;
    static const char *sqls[] = {
        "SELECT ?1 + 0", "SELECT ?1 + 1", "SELECT ?1 + 2", "SELECT ?1 + 3",
    };
    for (int i = 0; i < N_ITERS; i++) {
        int k = i % 4;
        char v[16];
        snprintf(v, sizeof(v), "%d", i);
        db_result_t *r = NULL;
        if (db_execute(db, sqls[k], (const char *[]){ v, NULL }, &r) != CUTILS_OK)
            return (void *)1;
        int got = atoi(r->rows[0][0]);
        db_result_free(r);
        if (got != i + k) return (void *)1;
    }
    return NULL;
}

static void test_threads_share_cache(void **state)
{
    cutils_db_t *db = *state;
    /* Smaller than the working set so threads also race on eviction. */
    assert_int_equal(db_stmt_cache_set_size(db, 3), CUTILS_OK);
    pthread_t th[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        assert_int_equal(pthread_create(&th[i], NULL, hammer, db), 0);
    for (int i = 0; i < N_THREADS; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        assert_null(ret);
    }
    db_stmt_cache_stats_t s = stats(db);
    assert_int_equal(s.hits + s.misses, N_THREADS * N_ITERS);
    assert_true(s.entries <= 3);
}

/* --- Typed parameters --- */

static void test_typed_blob_roundtrip(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE f (h BLOB PRIMARY KEY, size INTEGER, ratio REAL, note TEXT)"),
        CUTILS_OK);

    uint8_t hash[32];
    for (int i = 0; i < 32; i++) hash[i] = (uint8_t)(i * 37);
    hash[0] = 0;   /* embedded zero bytes must survive */
    hash[17] = 0;

    db_param_t ins[] = {
        DB_BLOB(hash, sizeof(hash)),
        DB_INT64(INT64_C(9007199254740993)),
        DB_DOUBLE(0.5),
        DB_NULLP,
    };
    int affected = 0;
    assert_int_equal(db_execute_non_query_typed(db,
        "INSERT INTO f VALUES (?, ?, ?, ?)", ins, 4, &affected), CUTILS_OK);
    assert_int_equal(affected, 1);

    db_param_t key[] = { DB_BLOB(hash, sizeof(hash)) };
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute_typed(db,
        "SELECT lower(hex(h)), length(h), typeof(h), size, ratio, note "
        "FROM f WHERE h = ?", key, 1, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0],
        "00254a6f94b9de03284d7297bce1062b50009abfe4092e53789dc2e70c31567b");
    assert_string_equal(r->rows[0][1], "32");
    assert_string_equal(r->rows[0][2], "blob");
    assert_string_equal(r->rows[0][3], "9007199254740993");
    assert_string_equal(r->rows[0][4], "0.5");
    assert_true(db_result_is_null(r, 0, 5));

    /* The same key as TEXT must not match the BLOB. */
    hash[0] = 1;
    CUTILS_AUTO_DBRES db_result_t *miss = NULL;
    assert_int_equal(db_execute_typed(db,
        "SELECT lower(hex(h)), length(h), typeof(h), size, ratio, note "
        "FROM f WHERE h = ?", key, 1, &miss), CUTILS_OK);
    assert_int_equal(miss->nrows, 0);
}

static void test_typed_text_and_edge_values(void **state)
{
    cutils_db_t *db = *state;
    db_param_t p[] = {
        DB_TEXT("hello"),
        { .type = DB_P_TEXT, .ptr = "hello world", .len = 5 },
        DB_TEXT(NULL),
        { .type = DB_P_BLOB, .ptr = NULL, .len = 0 },
    };
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute_typed(db,
        "SELECT ?1, ?2, ?3 IS NULL, typeof(?4), length(?4)", p, 4, &r),
        CUTILS_OK);
    assert_string_equal(r->rows[0][0], "hello");
    assert_string_equal(r->rows[0][1], "hello");
    assert_string_equal(r->rows[0][2], "1");
    assert_string_equal(r->rows[0][3], "blob");
    assert_string_equal(r->rows[0][4], "0");
}

static void test_typed_count_rules(void **state)
{
    cutils_db_t *db = *state;
    const char *sql = "SELECT ?1, ?2 IS NULL";
    db_param_t two[] = { DB_INT64(1), DB_INT64(2) };
    db_param_t three[] = { DB_INT64(1), DB_INT64(2), DB_INT64(3) };

    db_result_t *r = NULL;
    assert_int_equal(db_execute_typed(db, sql, three, 3, &r), CUTILS_ERR_DB);
    assert_null(r);
    assert_non_null(strstr(cutils_get_error(), "3 params for 2 placeholders"));

    assert_int_equal(db_execute_typed(db, sql, two, 2, &r), CUTILS_OK);
    assert_string_equal(r->rows[0][1], "0");
    db_result_free(r);

    /* Fewer than last time: the rest read as NULL on the cached stmt. */
    assert_int_equal(db_execute_typed(db, sql, two, 1, &r), CUTILS_OK);
    assert_string_equal(r->rows[0][0], "1");
    assert_string_equal(r->rows[0][1], "1");
    db_result_free(r);

    /* NULL array means no params, whatever the count says. */
    assert_int_equal(db_execute_typed(db, sql, NULL, 5, &r), CUTILS_OK);
    assert_true(db_result_is_null(r, 0, 0));
    db_result_free(r);

    assert_int_equal(db_execute_non_query_typed(db,
        "INSERT INTO t (name) VALUES (?)", NULL, 0, NULL), CUTILS_OK);
}

static void test_typed_bad_type(void **state)
{
    cutils_db_t *db = *state;
    db_param_t p[] = { { .type = (db_param_type_t)99 } };
    db_result_t *r = NULL;
    assert_int_equal(db_execute_typed(db, "SELECT ?", p, 1, &r), CUTILS_ERR_DB);
    assert_non_null(strstr(cutils_get_error(), "bind param 1"));
}

/* --- NULL detection --- */

static void test_result_is_null(void **state)
{
    cutils_db_t *db = *state;
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute(db,
        "SELECT NULL, '', 'x' UNION ALL SELECT 'y', NULL, ''", NULL, &r),
        CUTILS_OK);
    assert_int_equal(r->nrows, 2);
    assert_true(db_result_is_null(r, 0, 0));
    assert_false(db_result_is_null(r, 0, 1));
    assert_string_equal(r->rows[0][0], r->rows[0][1]);
    assert_false(db_result_is_null(r, 0, 2));
    assert_false(db_result_is_null(r, 1, 0));
    assert_true(db_result_is_null(r, 1, 1));
    assert_false(db_result_is_null(r, 1, 2));

    assert_false(db_result_is_null(r, -1, 0));
    assert_false(db_result_is_null(r, 2, 0));
    assert_false(db_result_is_null(r, 0, 3));
    assert_false(db_result_is_null(r, 0, -1));
    assert_false(db_result_is_null(NULL, 0, 0));
}

/* More rows than the initial row capacity, so the null map grows with
 * the row array. */
static void test_result_is_null_after_growth(void **state)
{
    cutils_db_t *db = *state;
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute(db,
        "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n "
        "WHERE i < 99) SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE i END "
        "FROM n", NULL, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 100);
    for (int i = 0; i < 100; i++) {
        assert_false(db_result_is_null(r, i, 0));
        assert_int_equal(db_result_is_null(r, i, 1), i % 3 == 0);
    }
}

static void test_result_is_null_zero_rows(void **state)
{
    cutils_db_t *db = *state;
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM t", NULL, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 0);
    assert_int_equal(r->ncols, 1);
    assert_false(db_result_is_null(r, 0, 0));
}

/* --- Close --- */

/* db_close must finalize cached statements, or sqlite3_close refuses
 * and the connection leaks; the sanitizer and valgrind runs catch the
 * leak. Reopening and writing proves the file was released. */
static void test_close_with_cached_statements(void **state)
{
    cutils_db_t *db = *state;
    insert(db, "a");
    assert_int_equal(count_rows(db), 1);
    assert_true(stats(db).entries >= 2);
    db_close(db);

    db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    insert(db, "b");
    assert_int_equal(count_rows(db), 2);
    *state = db;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
#define T(fn) cmocka_unit_test_setup_teardown(fn, setup, teardown)
        T(test_default_capacity),
        T(test_repeat_call_hits),
        T(test_once_bypasses_cache),
        T(test_omitted_params_read_null),
        T(test_rebind_replaces_value),
        T(test_lru_eviction),
        T(test_set_size_bounds_and_disable),
        T(test_shrink_keeps_most_recent),
        T(test_churn),
        T(test_schema_change_reprepares),
        T(test_dropped_table_fails_then_recovers),
        T(test_constraint_error_drops_entry),
        T(test_bind_error_drops_entry),
        T(test_prepare_error_not_cached),
        T(test_once_prepare_error),
        T(test_error_mid_result),
        T(test_non_query_rejects_rows),
        T(test_reset_releases_read_lock),
        T(test_rollback_with_cached_statements),
        T(test_threads_share_cache),
        T(test_typed_blob_roundtrip),
        T(test_typed_text_and_edge_values),
        T(test_typed_count_rules),
        T(test_typed_bad_type),
        T(test_result_is_null),
        T(test_result_is_null_after_growth),
        T(test_result_is_null_zero_rows),
        T(test_close_with_cached_statements),
#undef T
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
