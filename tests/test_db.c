#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
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

    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
    const char *p1[] = { "alice", NULL };
    const char *p2[] = { "bob", NULL };
    db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p1, NULL);
    db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p2, NULL);

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

    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "inside_txn", NULL };
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL);
    assert_int_equal(db_commit(db), CUTILS_OK);

    db_result_t *result = NULL;
    db_execute(db, "SELECT val FROM t", NULL, &result);
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

    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "should_vanish", NULL };
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL);
    assert_int_equal(db_rollback(db), CUTILS_OK);

    db_result_t *result = NULL;
    db_execute(db, "SELECT val FROM t", NULL, &result);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
    db_close(db);
}

static void test_savepoint(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
    db_begin(db);

    const char *p1[] = { "kept", NULL };
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p1, NULL);

    db_savepoint(db, "sp1");
    const char *p2[] = { "rolled_back", NULL };
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p2, NULL);
    db_savepoint_rollback(db, "sp1");

    db_commit(db);

    db_result_t *result = NULL;
    db_execute(db, "SELECT val FROM t", NULL, &result);
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
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)");

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT * FROM t", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
