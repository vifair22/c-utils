#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB "/tmp/cutils_test_db_err.db"

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

/* --- Error path tests --- */

static void test_exec_raw_bad_sql(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    int rc = db_exec_raw(db, "THIS IS NOT SQL");
    assert_int_not_equal(rc, CUTILS_OK);

    db_close(db);
}

static void test_execute_bad_sql(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    db_result_t *result = NULL;
    int rc = db_execute(db, "SELECT * FROM nonexistent_table", NULL, &result);
    assert_int_not_equal(rc, CUTILS_OK);
    assert_null(result);

    db_close(db);
}

static void test_execute_non_query_bad_sql(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    int rc = db_execute_non_query(db, "INSERT INTO nope VALUES (?)",
                                  (const char *[]){"x", NULL}, NULL);
    assert_int_not_equal(rc, CUTILS_OK);

    db_close(db);
}

static void test_execute_constraint_violation(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT UNIQUE)");

    const char *p1[] = { "alice", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t (name) VALUES (?)", p1, NULL), CUTILS_OK);

    /* Duplicate should fail */
    int rc = db_execute_non_query(db,
        "INSERT INTO t (name) VALUES (?)", p1, NULL);
    assert_int_not_equal(rc, CUTILS_OK);

    db_close(db);
}

static void test_close_null(void **state)
{
    (void)state;
    db_close(NULL); /* should not crash */
}

static void test_result_free_null(void **state)
{
    (void)state;
    db_result_free(NULL); /* should not crash */
}

static void test_execute_no_params(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

    /* INSERT with no params */
    int affected = 0;
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t (val) VALUES ('literal')", NULL, &affected), CUTILS_OK);
    assert_int_equal(affected, 1);

    /* SELECT with no params */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "literal");
    db_result_free(result);

    db_close(db);
}

static void test_execute_null_column_value(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (NULL)", NULL, NULL);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    /* NULL should be returned as empty string */
    assert_string_equal(result->rows[0][0], "");
    db_result_free(result);

    db_close(db);
}

static void test_execute_non_query_affected_null(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)");

    /* NULL affected pointer should be fine */
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO t DEFAULT VALUES", NULL, NULL), CUTILS_OK);

    db_close(db);
}

static void test_savepoint_release(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

    db_begin(db);
    db_savepoint(db, "sp1");
    const char *p[] = { "saved", NULL };
    db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL);
    assert_int_equal(db_savepoint_release(db, "sp1"), CUTILS_OK);
    db_commit(db);

    db_result_t *result = NULL;
    db_execute(db, "SELECT val FROM t", NULL, &result);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "saved");
    db_result_free(result);

    db_close(db);
}

static void test_multiple_rows(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

    /* Insert enough rows to trigger a realloc (> 16 initial capacity) */
    for (int i = 0; i < 20; i++) {
        char val[32];
        snprintf(val, sizeof(val), "row_%d", i);
        const char *p[] = { val, NULL };
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL);
    }

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t ORDER BY id", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 20);
    assert_string_equal(result->rows[0][0], "row_0");
    assert_string_equal(result->rows[19][0], "row_19");
    db_result_free(result);

    db_close(db);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_exec_raw_bad_sql, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_bad_sql, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_non_query_bad_sql, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_constraint_violation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_close_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_result_free_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_no_params, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_null_column_value, setup, teardown),
        cmocka_unit_test_setup_teardown(test_execute_non_query_affected_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_savepoint_release, setup, teardown),
        cmocka_unit_test_setup_teardown(test_multiple_rows, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
