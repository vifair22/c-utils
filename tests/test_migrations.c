#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB       "/tmp/cutils_mig_test.db"
#define TEST_MIG_DIR  "/tmp/cutils_mig_test_dir"

static int setup(void **state)
{
    (void)state;
    unlink(TEST_DB);
    /* Clean up migration dir */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_MIG_DIR);
    CUTILS_UNUSED(system(cmd));
    mkdir(TEST_MIG_DIR, 0755);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_MIG_DIR);
    CUTILS_UNUSED(system(cmd));
    return 0;
}

static void write_migration(const char *name, const char *sql)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TEST_MIG_DIR, name);
    FILE *f = fopen(path, "w");
    fprintf(f, "%s", sql);
    fclose(f);
}

static void test_lib_migrations_create_tables(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    /* Verify config table exists */
    db_result_t *result = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='config'",
                   NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    /* Verify logs table exists */
    result = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='logs'",
                   NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    /* Verify push table exists */
    result = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='push'",
                   NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    db_close(db);
}

static void test_lib_migrations_idempotent(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK); /* no-op */

    /* Count applied migrations */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT COUNT(*) FROM system_migrations WHERE filename LIKE '_lib/%'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    int count = atoi(result->rows[0][0]);
    assert_true(count >= 3); /* config, logs, push */
    db_result_free(result);

    db_close(db);
}

static void test_app_migrations(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    write_migration("001_users.sql",
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    write_migration("002_posts.sql",
        "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER);");

    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    /* Verify tables */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='users'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='posts'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    db_close(db);
}

static void test_app_migrations_idempotent(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    write_migration("001_users.sql",
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");

    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);
    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    db_close(db);
}

static void test_checksum_mismatch_fails(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    write_migration("001_users.sql",
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    /* Tamper with the file */
    write_migration("001_users.sql",
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT);");
    assert_int_not_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    db_close(db);
}

static void test_failed_migration_rolls_back(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    write_migration("001_good.sql",
        "CREATE TABLE good (id INTEGER PRIMARY KEY);");
    write_migration("002_bad.sql",
        "THIS IS NOT VALID SQL;");

    assert_int_not_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    /* good table should NOT exist — entire batch rolled back */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='good'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 0);
    db_result_free(result);

    db_close(db);
}

static void test_null_dir_is_noop(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_app_migrations(db, NULL), CUTILS_OK);
    db_close(db);
}

/* --- Compiled migration tests --- */

static const db_migration_t compiled_migrations[] = {
    {
        "001_widgets.sql",
        "CREATE TABLE widgets (id INTEGER PRIMARY KEY, label TEXT NOT NULL);"
    },
    {
        "002_gadgets.sql",
        "CREATE TABLE gadgets (id INTEGER PRIMARY KEY, widget_id INTEGER,"
        " FOREIGN KEY (widget_id) REFERENCES widgets(id));"
    },
    { NULL, NULL }
};

static void test_compiled_migrations(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(db_run_compiled_migrations(db, compiled_migrations), CUTILS_OK);

    /* Verify tables created */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='widgets'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='gadgets'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    db_close(db);
}

static void test_compiled_migrations_idempotent(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(db_run_compiled_migrations(db, compiled_migrations), CUTILS_OK);
    assert_int_equal(db_run_compiled_migrations(db, compiled_migrations), CUTILS_OK);

    /* Count — should be exactly 2 (no _lib/ prefix) */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db,
        "SELECT COUNT(*) FROM system_migrations WHERE filename NOT LIKE '_lib/%'",
        NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_int_equal(atoi(result->rows[0][0]), 2);
    db_result_free(result);

    db_close(db);
}

static void test_compiled_migrations_checksum_mismatch(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(db_run_compiled_migrations(db, compiled_migrations), CUTILS_OK);

    /* Run with same name but different SQL */
    static const db_migration_t tampered[] = {
        {
            "001_widgets.sql",
            "CREATE TABLE widgets (id INTEGER PRIMARY KEY, label TEXT, color TEXT);"
        },
        { NULL, NULL }
    };
    assert_int_not_equal(db_run_compiled_migrations(db, tampered), CUTILS_OK);

    db_close(db);
}

static void test_compiled_migrations_null_is_noop(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_compiled_migrations(db, NULL), CUTILS_OK);
    db_close(db);
}

/* Empty .sql files in the migrations directory are skipped silently
 * rather than failing the run. Useful for the case where a developer
 * `touch`'d a new migration file before writing its SQL — the test
 * here also exercises the fsize == 0 branch added in 1.1.0 to make
 * that skip explicit. */
static void test_empty_sql_file_is_skipped(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    write_migration("001_real.sql",
        "CREATE TABLE real_table (id INTEGER PRIMARY KEY);");
    write_migration("002_empty.sql", "");

    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    /* Real table exists. */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='real_table'",
        NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    /* Empty migration is not recorded as applied (no checksum row). */
    result = NULL;
    const char *p[] = { "002_empty.sql", NULL };
    assert_int_equal(db_execute(db,
        "SELECT filename FROM system_migrations WHERE filename = ?",
        p, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 0);
    db_result_free(result);

    db_close(db);
}

static void test_compiled_and_file_migrations_coexist(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    /* Run compiled migrations first */
    assert_int_equal(db_run_compiled_migrations(db, compiled_migrations), CUTILS_OK);

    /* Then file-based migrations */
    write_migration("003_extras.sql",
        "CREATE TABLE extras (id INTEGER PRIMARY KEY, data TEXT);");
    assert_int_equal(db_run_app_migrations(db, TEST_MIG_DIR), CUTILS_OK);

    /* All three app tables should exist */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='widgets'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    result = NULL;
    assert_int_equal(db_execute(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='extras'",
               NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    db_close(db);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_lib_migrations_create_tables, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lib_migrations_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_app_migrations, setup, teardown),
        cmocka_unit_test_setup_teardown(test_app_migrations_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_checksum_mismatch_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_failed_migration_rolls_back, setup, teardown),
        cmocka_unit_test_setup_teardown(test_null_dir_is_noop, setup, teardown),
        cmocka_unit_test_setup_teardown(test_compiled_migrations, setup, teardown),
        cmocka_unit_test_setup_teardown(test_compiled_migrations_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_compiled_migrations_checksum_mismatch, setup, teardown),
        cmocka_unit_test_setup_teardown(test_compiled_migrations_null_is_noop, setup, teardown),
        cmocka_unit_test_setup_teardown(test_compiled_and_file_migrations_coexist, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_sql_file_is_skipped, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
