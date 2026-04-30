#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB "/tmp/cutils_test.db"

static int setup(void **state)
{
    (void)state;
    unlink(TEST_DB);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static void test_open_close(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    int rc = db_open(&db, TEST_DB);
    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(db);
    db_close(db);
}

static void test_create_table_and_insert(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"), CUTILS_OK);

    const char *params[] = { "hello", NULL };
    int affected = 0;
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)",
                             params, &affected),
        CUTILS_OK);
    assert_int_equal(affected, 1);

    db_close(db);
}

static void test_select(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"),
        CUTILS_OK);
    const char *p1[] = { "alice", NULL };
    const char *p2[] = { "bob", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p1, NULL),
        CUTILS_OK);
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p2, NULL),
        CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM t ORDER BY name", NULL, &result),
        CUTILS_OK);

    assert_non_null(result);
    assert_int_equal(result->nrows, 2);
    assert_int_equal(result->ncols, 1);
    assert_string_equal(result->rows[0][0], "alice");
    assert_string_equal(result->rows[1][0], "bob");
    assert_string_equal(result->col_names[0], "name");

    db_result_free(result);
    db_close(db);
}

static void test_transaction_commit(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "inside_txn", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL),
        CUTILS_OK);
    assert_int_equal(db_commit(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "inside_txn");

    db_result_free(result);
    db_close(db);
}

static void test_transaction_rollback(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "should_vanish", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL),
        CUTILS_OK);
    assert_int_equal(db_rollback(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
    db_close(db);
}

static void test_savepoint(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);
    assert_int_equal(db_begin(db), CUTILS_OK);

    const char *p1[] = { "kept", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p1, NULL),
        CUTILS_OK);

    assert_int_equal(db_savepoint(db, "sp1"), CUTILS_OK);
    const char *p2[] = { "rolled_back", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p2, NULL),
        CUTILS_OK);
    assert_int_equal(db_savepoint_rollback(db, "sp1"), CUTILS_OK);

    assert_int_equal(db_commit(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "kept");

    db_result_free(result);
    db_close(db);
}

static void test_empty_result(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)"),
        CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT * FROM t", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
    db_close(db);
}

static void test_auto_dbres_frees_on_scope_exit(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE t (v TEXT)"), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "INSERT INTO t VALUES ('auto')"),
                     CUTILS_OK);

    {
        CUTILS_AUTO_DBRES db_result_t *r = NULL;
        assert_int_equal(db_execute(db, "SELECT v FROM t", NULL, &r), CUTILS_OK);
        assert_non_null(r);
        assert_int_equal(r->nrows, 1);
        assert_string_equal(r->rows[0][0], "auto");
        /* No explicit db_result_free — cleanup fires on block exit.
         * ASAN validates under make test-asan. */
    }

    db_close(db);
}

static void test_auto_dbres_null_safe(void **state)
{
    (void)state;
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    (void)r;
    /* Cleanup must tolerate NULL without crashing. */
}

/* --- CUTILS_AUTO_DB_TX --- */

static int count_rows(cutils_db_t *db)
{
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT COUNT(*) FROM tx", NULL, &r),
                     CUTILS_OK);
    int n = atoi(r->rows[0][0]);
    db_result_free(r);
    return n;
}

static void test_auto_db_tx_commit_persists(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('committed')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_rolls_back_on_early_return(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    for (int i = 0; i < 1; i++) {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('dropped')"), CUTILS_OK);
        break;  /* scope exits without commit — cleanup rolls back */
    }

    assert_int_equal(count_rows(db), 0);
    db_close(db);
}

static void test_auto_db_tx_commit_is_idempotent(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('once')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
        /* Second commit is a no-op — must not error. */
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_immediate_commit_persists(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin_immediate(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('committed')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_immediate_rolls_back_on_early_return(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    for (int i = 0; i < 1; i++) {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin_immediate(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('dropped')"), CUTILS_OK);
        break;
    }

    assert_int_equal(count_rows(db), 0);
    db_close(db);
}


/* --- Concurrency regression: tx scope must hold the mutex --- */

typedef struct {
    cutils_db_t *db;
} race_arg_t;

static void *race_tx_thread(void *p)
{
    race_arg_t *a = p;
    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(a->db, &tx), CUTILS_OK);
        const char *params[] = { "a", NULL };
        assert_int_equal(
            db_execute_non_query(a->db, "INSERT INTO race (v) VALUES (?)",
                                 params, NULL), CUTILS_OK);
        /* Hold the tx open long enough for the writer thread to attempt
         * its INSERT. Without the lock-held-across-tx fix, that INSERT
         * lands inside this tx and is rolled back along with row 'a'. */
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        /* Falls out of scope without commit — auto-cleanup rolls back. */
    }
    return NULL;
}

static void *race_writer_thread(void *p)
{
    race_arg_t *a = p;
    const char *params[] = { "b", NULL };
    assert_int_equal(
        db_execute_non_query(a->db, "INSERT INTO race (v) VALUES (?)",
                             params, NULL), CUTILS_OK);
    return NULL;
}

static void test_tx_holds_mutex_against_concurrent_writer(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE race (v TEXT)"), CUTILS_OK);

    race_arg_t arg = { .db = db };
    pthread_t tA, tB;

    pthread_create(&tA, NULL, race_tx_thread, &arg);

    /* Let tA enter its tx scope before tB starts. */
    struct timespec gap = { 0, 50 * 1000 * 1000 };
    nanosleep(&gap, NULL);

    pthread_create(&tB, NULL, race_writer_thread, &arg);

    pthread_join(tA, NULL);
    pthread_join(tB, NULL);

    /* Only tB's row should survive — tA rolled back. */
    db_result_t *r = NULL;
    assert_int_equal(
        db_execute(db, "SELECT v FROM race", NULL, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0], "b");
    db_result_free(r);

    db_close(db);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_open_close, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_table_and_insert, setup, teardown),
        cmocka_unit_test_setup_teardown(test_select, setup, teardown),
        cmocka_unit_test_setup_teardown(test_transaction_commit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_transaction_rollback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_savepoint, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_result, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_dbres_frees_on_scope_exit, setup, teardown),
        cmocka_unit_test(test_auto_dbres_null_safe),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_commit_persists, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_rolls_back_on_early_return, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_commit_is_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_immediate_commit_persists, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_immediate_rolls_back_on_early_return, setup, teardown),
        cmocka_unit_test_setup_teardown(test_tx_holds_mutex_against_concurrent_writer, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
