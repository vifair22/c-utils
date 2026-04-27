#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_CFG  "/tmp/cutils_test_config.yaml"
#define TEST_DB   "/tmp/cutils_test_config.db"

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_CFG);
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(content, f);
    fclose(f);
}

/* --- Init tests --- */

static void test_init_basic(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n");

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_CONTINUE, NULL, NULL);
    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(cfg);

    /* Internal key should be registered */
    assert_true(config_has_key(cfg, "db.path"));
    assert_string_equal(config_get_str(cfg, "db.path"), "test.db");

    config_free(cfg);
}

static void test_init_with_app_keys(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n"
        "app:\n"
        "  port: 9090\n"
        "  name: myapp\n");

    const config_key_t app_keys[] = {
        { "app.port", CFG_INT, "8080", "Listen port", CFG_STORE_FILE, 0 },
        { "app.name", CFG_STRING, "default", "App name", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    const config_section_t sections[] = {
        { "app", "Application" },
        { NULL, NULL }
    };

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_CONTINUE, app_keys, sections);
    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(cfg);

    assert_string_equal(config_get_str(cfg, "app.port"), "9090");
    assert_string_equal(config_get_str(cfg, "app.name"), "myapp");
    assert_int_equal(config_get_int(cfg, "app.port", 0), 9090);

    config_free(cfg);
}

static void test_init_first_run_generates_template(void **state)
{
    (void)state;
    /* No config file exists — should generate template and return ERR_NOT_FOUND */
    unlink(TEST_CFG);

    const config_key_t keys[] = {
        { "app.name", CFG_STRING, "myapp", "App name", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_EXIT, keys, NULL);
    assert_int_equal(rc, CUTILS_ERR_NOT_FOUND);
    assert_null(cfg);

    /* Template file should exist now */
    FILE *f = fopen(TEST_CFG, "r");
    assert_non_null(f);
    fclose(f);
}

static void test_init_first_run_continue(void **state)
{
    (void)state;
    unlink(TEST_CFG);

    const config_key_t keys[] = {
        { "app.name", CFG_STRING, "myapp", "App name", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_CONTINUE, keys, NULL);
    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(cfg);

    /* Should use default value */
    assert_string_equal(config_get_str(cfg, "app.name"), "myapp");

    config_free(cfg);
}

static void test_init_required_key_missing(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n"
        "app:\n"
        "  name:\n");

    const config_key_t keys[] = {
        { "app.name", CFG_STRING, "", "Required key", CFG_STORE_FILE, 1 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_CONTINUE, keys, NULL);
    assert_int_equal(rc, CUTILS_ERR_CONFIG);
}

static void test_init_duplicate_key(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    /* db.path is already an internal key — registering it again should fail */
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, "dupe", "Dupe", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "testapp", TEST_CFG,
                         CFG_FIRST_RUN_CONTINUE, keys, NULL);
    assert_int_equal(rc, CUTILS_ERR_EXISTS);
}

/* --- Get API --- */

static void test_get_int(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n"
        "app:\n"
        "  port: 8080\n"
        "  bad: notanumber\n"
        "  empty:\n");

    const config_key_t keys[] = {
        { "app.port", CFG_INT, "3000", "Port", CFG_STORE_FILE, 0 },
        { "app.bad", CFG_STRING, "", "", CFG_STORE_FILE, 0 },
        { "app.empty", CFG_STRING, "", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    assert_int_equal(config_get_int(cfg, "app.port", 99), 8080);
    assert_int_equal(config_get_int(cfg, "app.bad", 42), 42);
    assert_int_equal(config_get_int(cfg, "app.empty", 77), 77);
    assert_int_equal(config_get_int(cfg, "app.nonexist", 55), 55);

    config_free(cfg);
}

static void test_get_bool(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n"
        "flags:\n"
        "  a: true\n"
        "  b: false\n"
        "  c: 1\n"
        "  d: 0\n"
        "  e: yes\n"
        "  f: no\n"
        "  g: maybe\n"
        "  h:\n");

    const config_key_t keys[] = {
        { "flags.a", CFG_BOOL, "false", "", CFG_STORE_FILE, 0 },
        { "flags.b", CFG_BOOL, "true", "", CFG_STORE_FILE, 0 },
        { "flags.c", CFG_BOOL, "false", "", CFG_STORE_FILE, 0 },
        { "flags.d", CFG_BOOL, "true", "", CFG_STORE_FILE, 0 },
        { "flags.e", CFG_BOOL, "false", "", CFG_STORE_FILE, 0 },
        { "flags.f", CFG_BOOL, "true", "", CFG_STORE_FILE, 0 },
        { "flags.g", CFG_BOOL, "false", "", CFG_STORE_FILE, 0 },
        { "flags.h", CFG_BOOL, "false", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    assert_int_equal(config_get_bool(cfg, "flags.a", 0), 1);
    assert_int_equal(config_get_bool(cfg, "flags.b", 1), 0);
    assert_int_equal(config_get_bool(cfg, "flags.c", 0), 1);
    assert_int_equal(config_get_bool(cfg, "flags.d", 1), 0);
    assert_int_equal(config_get_bool(cfg, "flags.e", 0), 1);
    assert_int_equal(config_get_bool(cfg, "flags.f", 1), 0);
    assert_int_equal(config_get_bool(cfg, "flags.g", 99), 99); /* unrecognized */
    assert_int_equal(config_get_bool(cfg, "flags.h", 77), 0);  /* empty -> default_value "false" -> 0 */

    config_free(cfg);
}

static void test_get_str_default(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: test.db\n"
        "app:\n"
        "  empty:\n");

    const config_key_t keys[] = {
        { "app.empty", CFG_STRING, "fallback", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    /* Empty value in file -> falls through to compiled-in default */
    assert_string_equal(config_get_str(cfg, "app.empty"), "fallback");

    /* Unknown key returns NULL */
    assert_null(config_get_str(cfg, "totally.unknown"));

    config_free(cfg);
}

static void test_get_env_override(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: file_value.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Set env var: TESTAPP_DB_PATH */
    setenv("TESTAPP_DB_PATH", "env_value.db", 1);
    assert_string_equal(config_get_str(cfg, "db.path"), "env_value.db");
    unsetenv("TESTAPP_DB_PATH");

    /* Without env, should use file value */
    assert_string_equal(config_get_str(cfg, "db.path"), "file_value.db");

    config_free(cfg);
}

/* --- Mutation API --- */

static void test_set_file_key(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: old.db\n"
        "app:\n"
        "  name: oldname\n");

    const config_key_t keys[] = {
        { "app.name", CFG_STRING, "default", "Name", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    assert_int_equal(config_set(cfg, "app.name", "newname"), CUTILS_OK);
    assert_string_equal(config_get_str(cfg, "app.name"), "newname");

    config_free(cfg);
}

static void test_set_internal_key_fails(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* db.path is an internal minimum, should refuse mutation */
    assert_int_not_equal(config_set(cfg, "db.path", "hacked.db"), CUTILS_OK);

    config_free(cfg);
}

static void test_set_unknown_key_fails(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    assert_int_equal(config_set(cfg, "nope.key", "val"), CUTILS_ERR_NOT_FOUND);

    config_free(cfg);
}

/* --- Key count / query API --- */

static void test_key_counts(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    const config_key_t keys[] = {
        { "app.x", CFG_STRING, "", "", CFG_STORE_FILE, 0 },
        { "app.y", CFG_STRING, "", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    /* 1 internal + 2 app = 3 file keys */
    assert_int_equal(config_file_key_count(cfg), 3);
    assert_int_equal(config_db_key_count(cfg), 0);

    assert_true(config_has_key(cfg, "app.x"));
    assert_false(config_has_key(cfg, "app.z"));

    const config_key_t *def = config_get_key_def(cfg, "app.x");
    assert_non_null(def);
    assert_string_equal(def->key, "app.x");

    assert_null(config_get_key_def(cfg, "nope"));

    config_free(cfg);
}

/* --- DB config integration --- */

static void test_attach_db(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Open DB and run lib migrations to create config table */
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "runtime.interval", CFG_INT, "60", "Poll interval",
          CFG_STORE_DB, 0 },
        { "runtime.enabled", CFG_BOOL, "true", "Enabled flag",
          CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);
    assert_int_equal(config_db_key_count(cfg), 2);

    /* Default values should be seeded */
    assert_string_equal(config_get_str(cfg, "runtime.interval"), "60");
    assert_int_equal(config_get_int(cfg, "runtime.interval", 0), 60);
    assert_int_equal(config_get_bool(cfg, "runtime.enabled", 0), 1);

    /* Mutate DB key */
    assert_int_equal(config_set_db(cfg, "runtime.interval", "120"), CUTILS_OK);
    assert_string_equal(config_get_str(cfg, "runtime.interval"), "120");

    /* Attaching again with same keys fails (duplicate key registration) */
    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_ERR_EXISTS);

    db_close(db);
    config_free(cfg);
}

static void test_set_db_errors(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    const config_key_t file_keys[] = {
        { "app.name", CFG_STRING, "x", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, file_keys, NULL), CUTILS_OK);

    /* set_db without DB attached */
    assert_int_equal(config_set_db(cfg, "app.name", "x"), CUTILS_ERR_INVALID);

    /* Attach DB */
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "runtime.x", CFG_STRING, "", "", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);

    /* set_db on file-backed key */
    assert_int_equal(config_set_db(cfg, "app.name", "x"), CUTILS_ERR_INVALID);

    /* set_db on unknown key */
    assert_int_equal(config_set_db(cfg, "nope.key", "x"), CUTILS_ERR_NOT_FOUND);

    /* config_set on DB-backed key */
    assert_int_equal(config_set(cfg, "runtime.x", "x"), CUTILS_ERR_INVALID);

    db_close(db);
    config_free(cfg);
}

static void test_attach_db_null_keys(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    /* NULL db_keys is valid — just attaches the DB handle */
    assert_int_equal(config_attach_db(cfg, db, NULL), CUTILS_OK);

    db_close(db);
    config_free(cfg);
}

/* --- Mixed store keys in config_init (FILE + DB keys in same array) --- */

static void test_init_skips_db_keys(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n  app:\n    x: val\n");

    /* Pass a mix of FILE and DB keys to config_init — DB keys should be skipped */
    const config_key_t keys[] = {
        { "app.x", CFG_STRING, "", "", CFG_STORE_FILE, 0 },
        { "app.dbonly", CFG_STRING, "d", "", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, NULL), CUTILS_OK);

    /* app.x should be registered, app.dbonly should NOT be registered yet */
    assert_true(config_has_key(cfg, "app.x"));
    assert_false(config_has_key(cfg, "app.dbonly"));
    assert_int_equal(config_file_key_count(cfg), 2); /* db.path + app.x */

    config_free(cfg);
}

/* --- DB key access before/after attach --- */

static void test_get_db_key_no_db(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* db.path is file-backed with default — should return from file */
    assert_string_equal(config_get_str(cfg, "db.path"), "test.db");

    /* Non-existent key returns NULL */
    assert_null(config_get_str(cfg, "runtime.bogus"));

    config_free(cfg);
}

/* --- Attach DB with mixed keys (includes FILE key in db_keys) --- */

static void test_attach_db_skips_file_keys(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "runtime.x", CFG_STRING, "val", "", CFG_STORE_DB, 0 },
        { "skip.this", CFG_STRING, "x", "", CFG_STORE_FILE, 0 }, /* should be skipped */
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);
    assert_true(config_has_key(cfg, "runtime.x"));
    assert_false(config_has_key(cfg, "skip.this")); /* FILE key skipped in attach */

    db_close(db);
    config_free(cfg);
}

/* --- DB key with missing value falls through to default --- */

static void test_get_db_key_empty_value(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "runtime.empty", CFG_STRING, "fallback", "", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);

    /* Clear the value in DB to force fallback to default */
    const char *params[] = { "", "runtime.empty", NULL };
    assert_int_equal(db_execute_non_query(db, "UPDATE config SET value = ? WHERE key = ?", params, NULL), CUTILS_OK);

    /* Should fall through to compiled-in default */
    assert_string_equal(config_get_str(cfg, "runtime.empty"), "fallback");

    db_close(db);
    config_free(cfg);
}

/* --- config_get_int/bool with NULL from get_str --- */

static void test_get_int_unknown_key(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Unknown key — config_get_str returns NULL */
    assert_int_equal(config_get_int(cfg, "nope.nope", -1), -1);
    assert_int_equal(config_get_bool(cfg, "nope.nope", -1), -1);

    config_free(cfg);
}

/* --- Long error message in set_error_errno to overflow buffer --- */

static void test_error_errno_long_message(void **state)
{
    (void)state;
    /* Create a message that nearly fills the buffer to test the overflow branch */
    char long_fmt[600];
    memset(long_fmt, 'X', sizeof(long_fmt) - 1);
    long_fmt[sizeof(long_fmt) - 1] = '\0';

    errno = ENOENT;
    set_error_errno(CUTILS_ERR_IO, "%s", long_fmt);
    const char *msg = cutils_get_error();
    /* Message should be truncated but not crash */
    assert_non_null(msg);
    assert_true(strlen(msg) > 0);
}

/* --- config_free NULL safety --- */

static void test_free_null(void **state)
{
    (void)state;
    config_free(NULL); /* should not crash */
}

/* --- Env var prefix construction --- */

static void test_env_prefix_special_chars(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    /* App name with hyphens and dots */
    assert_int_equal(config_init(&cfg, "my-cool.app", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Env var should be MY_COOL_APP_DB_PATH */
    setenv("MY_COOL_APP_DB_PATH", "env.db", 1);
    assert_string_equal(config_get_str(cfg, "db.path"), "env.db");
    unsetenv("MY_COOL_APP_DB_PATH");

    config_free(cfg);
}

/* --- Duplicate section handling --- */

static void test_duplicate_section(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    const config_section_t sections[] = {
        { "db", "Database" },
        { "db", "Database Again" },
        { NULL, NULL }
    };

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, sections), CUTILS_OK);
    config_free(cfg);
}

/* --- DB key type strings --- */

static void test_db_key_types(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "rt.str", CFG_STRING, "hello", "A string", CFG_STORE_DB, 0 },
        { "rt.num", CFG_INT, "42", "A number", CFG_STORE_DB, 0 },
        { "rt.flag", CFG_BOOL, "true", "A bool", CFG_STORE_DB, 0 },
        { "rt.bare", CFG_STRING, NULL, NULL, CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);

    /* Check types were stored correctly */
    db_result_t *result = NULL;
    const char *p[] = { "rt.num", NULL };
    assert_int_equal(db_execute(db, "SELECT type FROM config WHERE key = ?",
                                p, &result), CUTILS_OK);
    assert_string_equal(result->rows[0][0], "int");
    db_result_free(result);

    const char *p2[] = { "rt.flag", NULL };
    assert_int_equal(db_execute(db, "SELECT type FROM config WHERE key = ?",
                                p2, &result), CUTILS_OK);
    assert_string_equal(result->rows[0][0], "bool");
    db_result_free(result);

    db_close(db);
    config_free(cfg);
}

/* Regression test: a previous implementation of config_get_from_db wrote
 * into a single shared thread-local buffer, so reading two DB-backed
 * keys in a row silently aliased the pointers — the saved pointer to
 * the first value ended up pointing at the second key's value after the
 * second read. This test pins the correct behavior. */
static void test_get_db_str_stable_across_reads(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "mode",  CFG_STRING, "command", "", CFG_STORE_DB, 0 },
        { "delay", CFG_STRING, "5",       "", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);

    assert_int_equal(config_set_db(cfg, "mode",  "register"), CUTILS_OK);
    assert_int_equal(config_set_db(cfg, "delay", "120"),      CUTILS_OK);

    /* The aliasing bug shape: save the first read, do a second read,
     * assert the first pointer still reads the first key's value. */
    const char *mode = config_get_str(cfg, "mode");
    assert_non_null(mode);
    assert_string_equal(mode, "register");

    const char *delay = config_get_str(cfg, "delay");
    assert_non_null(delay);
    assert_string_equal(delay, "120");

    /* After the delay read, mode's pointer must still resolve to
     * "register" — not "120". */
    assert_string_equal(mode, "register");

    /* And the two reads must hand back distinct pointers, not aliases
     * into a shared buffer. */
    assert_ptr_not_equal(mode, delay);

    /* config_set_db invalidates the cache, and a subsequent read returns
     * the refreshed value. The returned pointer may differ from the
     * previous one (new strdup), but the slot remains stable. */
    assert_int_equal(config_set_db(cfg, "mode", "none"), CUTILS_OK);
    const char *mode2 = config_get_str(cfg, "mode");
    assert_string_equal(mode2, "none");

    db_close(db);
    config_free(cfg);
}

/* <APP>_CONFIG_PATH env var resolves the YAML path when the caller
 * passes config_path=NULL. An explicit non-NULL config_path arg always
 * wins — the env var is a fallback, not an override of caller intent.
 * Both ordering matter: containers/packages set the env var to relocate
 * the file; tests and apps that pin a path keep working unchanged. */
#define TEST_CFG_ENV "/tmp/cutils_test_config_envpath.yaml"

static int teardown_envpath(void **state)
{
    (void)state;
    unsetenv("TESTAPP_CONFIG_PATH");
    unlink(TEST_CFG_ENV);
    unlink(TEST_CFG);
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static void test_init_env_config_path(void **state)
{
    (void)state;
    write_file(TEST_CFG_ENV,
        "db:\n"
        "  path: env_path.db\n");
    write_file(TEST_CFG,
        "db:\n"
        "  path: arg_path.db\n");

    /* Env var takes effect when caller passes NULL. */
    setenv("TESTAPP_CONFIG_PATH", TEST_CFG_ENV, 1);
    cutils_config_t *cfg_env = NULL;
    assert_int_equal(config_init(&cfg_env, "testapp", NULL,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL),
                     CUTILS_OK);
    assert_string_equal(config_get_str(cfg_env, "db.path"), "env_path.db");
    config_free(cfg_env);

    /* Explicit config_path arg wins over the env var. */
    cutils_config_t *cfg_arg = NULL;
    assert_int_equal(config_init(&cfg_arg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL),
                     CUTILS_OK);
    assert_string_equal(config_get_str(cfg_arg, "db.path"), "arg_path.db");
    config_free(cfg_arg);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_init_basic, teardown),
        cmocka_unit_test_teardown(test_init_with_app_keys, teardown),
        cmocka_unit_test_teardown(test_init_first_run_generates_template, teardown),
        cmocka_unit_test_teardown(test_init_first_run_continue, teardown),
        cmocka_unit_test_teardown(test_init_required_key_missing, teardown),
        cmocka_unit_test_teardown(test_init_duplicate_key, teardown),
        cmocka_unit_test_teardown(test_get_int, teardown),
        cmocka_unit_test_teardown(test_get_bool, teardown),
        cmocka_unit_test_teardown(test_get_str_default, teardown),
        cmocka_unit_test_teardown(test_get_env_override, teardown),
        cmocka_unit_test_teardown(test_set_file_key, teardown),
        cmocka_unit_test_teardown(test_set_internal_key_fails, teardown),
        cmocka_unit_test_teardown(test_set_unknown_key_fails, teardown),
        cmocka_unit_test_teardown(test_key_counts, teardown),
        cmocka_unit_test_teardown(test_attach_db, teardown),
        cmocka_unit_test_teardown(test_set_db_errors, teardown),
        cmocka_unit_test_teardown(test_attach_db_null_keys, teardown),
        cmocka_unit_test_teardown(test_init_skips_db_keys, teardown),
        cmocka_unit_test_teardown(test_get_db_key_no_db, teardown),
        cmocka_unit_test_teardown(test_attach_db_skips_file_keys, teardown),
        cmocka_unit_test_teardown(test_get_db_key_empty_value, teardown),
        cmocka_unit_test_teardown(test_get_int_unknown_key, teardown),
        cmocka_unit_test_teardown(test_error_errno_long_message, teardown),
        cmocka_unit_test_teardown(test_free_null, teardown),
        cmocka_unit_test_teardown(test_env_prefix_special_chars, teardown),
        cmocka_unit_test_teardown(test_duplicate_section, teardown),
        cmocka_unit_test_teardown(test_db_key_types, teardown),
        cmocka_unit_test_teardown(test_get_db_str_stable_across_reads, teardown),
        cmocka_unit_test_teardown(test_init_env_config_path, teardown_envpath),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
