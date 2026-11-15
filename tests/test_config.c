#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    /* chmod 0600 — tests using this helper produce config files
     * holding stand-in credentials; restrictive perms keep the test
     * tree tidy and match what a real-world app would have on the
     * file. Tests that need a specific mode chmod the file
     * themselves after this returns. */
    chmod(path, 0600);
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

/* Teardown that also clears env vars used by env-aware tests. Without
 * this, a failed assertion leaks the env var into subsequent tests
 * whose config_init then captures it — cascading failures that
 * obscure the real one. */
static int teardown_env(void **state)
{
    (void)state;
    unsetenv("TESTAPP_DB_PATH");
    unsetenv("MY_COOL_APP_DB_PATH");
    unsetenv("TESTAPP_CONFIG_PATH");
    unlink(TEST_CFG);
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static void test_get_env_override(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: file_value.db\n");

    /* Set env BEFORE config_init — values are snapshotted at
     * registration time, not on every read. */
    setenv("TESTAPP_DB_PATH", "env_value.db", 1);

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Env-captured value wins over file value. */
    assert_string_equal(config_get_str(cfg, "db.path"), "env_value.db");

    /* Changing the env after init has no effect — the snapshot is
     * frozen for the config's lifetime. */
    setenv("TESTAPP_DB_PATH", "different_value.db", 1);
    assert_string_equal(config_get_str(cfg, "db.path"), "env_value.db");

    /* Unsetting after init likewise has no effect. */
    unsetenv("TESTAPP_DB_PATH");
    assert_string_equal(config_get_str(cfg, "db.path"), "env_value.db");

    config_free(cfg);
}

/* Verifies the captured-at-init contract from the opposite direction:
 * a config initialized with no env set never picks up a later setenv,
 * even though the env var name matches and was set well before any
 * read. This is the property that closes the POSIX setenv-vs-getenv
 * race documented in config.h. */
static void test_env_not_observed_after_init(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: file_value.db\n");

    /* No env at init time. */
    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    /* Now set the env — must not be observed. */
    setenv("TESTAPP_DB_PATH", "late_value.db", 1);
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

    /* Env var should be MY_COOL_APP_DB_PATH — set before init so the
     * captured-at-registration snapshot picks it up. */
    setenv("MY_COOL_APP_DB_PATH", "env.db", 1);

    cutils_config_t *cfg = NULL;
    /* App name with hyphens and dots */
    assert_int_equal(config_init(&cfg, "my-cool.app", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL), CUTILS_OK);

    assert_string_equal(config_get_str(cfg, "db.path"), "env.db");

    config_free(cfg);
    unsetenv("MY_COOL_APP_DB_PATH");
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

/* --- Concurrency regression: shared cfg across threads --- */

typedef struct {
    cutils_config_t *cfg;
    int              iterations;
} concurrent_cfg_arg_t;

static void *concurrent_db_reader(void *p)
{
    concurrent_cfg_arg_t *a = p;
    for (int i = 0; i < a->iterations; i++) {
        const char *v = config_get_str(a->cfg, "feature.flag");
        /* Value may be the seeded default "off" or whatever the writer
         * just set — both are non-NULL and short. The point is we're
         * mutating the cache list and reading it concurrently; without
         * the mutex this UAFs or returns torn list pointers. */
        assert_non_null(v);
        (void)v;
    }
    return NULL;
}

static void *concurrent_db_writer(void *p)
{
    concurrent_cfg_arg_t *a = p;
    for (int i = 0; i < a->iterations; i++) {
        const char *new_val = (i & 1) ? "on" : "off";
        assert_int_equal(config_set_db(a->cfg, "feature.flag", new_val),
                         CUTILS_OK);
    }
    return NULL;
}

static void test_concurrent_cfg_db_readers_writers(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");

    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init(&cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, NULL, NULL),
                     CUTILS_OK);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE config (key TEXT PRIMARY KEY, value TEXT, type TEXT, "
        "default_value TEXT, description TEXT)"), CUTILS_OK);

    const config_key_t db_keys[] = {
        { "feature.flag", CFG_STRING, "off", "Toggle", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    assert_int_equal(config_attach_db(cfg, db, db_keys), CUTILS_OK);

    enum { NREADERS = 4, NWRITERS = 2, ITERATIONS = 200 };
    pthread_t readers[NREADERS], writers[NWRITERS];
    concurrent_cfg_arg_t arg = { .cfg = cfg, .iterations = ITERATIONS };

    for (int i = 0; i < NREADERS; i++)
        pthread_create(&readers[i], NULL, concurrent_db_reader, &arg);
    for (int i = 0; i < NWRITERS; i++)
        pthread_create(&writers[i], NULL, concurrent_db_writer, &arg);

    for (int i = 0; i < NREADERS; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < NWRITERS; i++) pthread_join(writers[i], NULL);

    /* Final value is one of the writer's values — both are valid. */
    const char *final = config_get_str(cfg, "feature.flag");
    assert_non_null(final);
    assert_true(strcmp(final, "on") == 0 || strcmp(final, "off") == 0);

    db_close(db);
    config_free(cfg);
}

/* --- Validation (1.4.0) ---
 *
 * Each of these loaded silently before: the daemon started and did
 * something other than what the file said. */

static const config_key_t v_keys[] = {
    { "security.key_dir", CFG_STRING, "/etc/keys", "Keys",    CFG_STORE_FILE, 0 },
    { "http.port",        CFG_INT,    "8765",      "Port",    CFG_STORE_FILE, 0 },
    { "http.secure",      CFG_BOOL,   "false",     "Secure",  CFG_STORE_FILE, 0 },
    { "ui.theme",         CFG_STRING, "dark",      "Theme",   CFG_STORE_DB,   0 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

static int init_v(cutils_config_t **cfg, config_validate_t mode)
{
    config_init_opts_t o = { .validate = mode };
    return config_init_opts(cfg, "testapp", TEST_CFG, CFG_FIRST_RUN_CONTINUE,
                            v_keys, NULL, &o);
}

static void assert_strict_error(const char *yaml, const char *needle)
{
    write_file(TEST_CFG, yaml);
    cutils_config_t *cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_STRICT), CUTILS_ERR_CONFIG);
    assert_null(cfg);
    const char *err = cutils_get_error();
    if (!strstr(err, needle))
        fail_msg("error '%s' lacks '%s'", err, needle);
}

static void test_validate_clean_file_passes(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "---\n"
        "# comment\n"
        "db:\n"
        "  path: test.db  # trailing\n"
        "security:\n"
        "  key_dir: \"/srv/keys # not a comment\"\n"
        "http:\n"
        "  port: 9000\n"
        "  secure: yes\n"
        "log:\n"
        "  level: debug\n"
        "pushover:\n"
        "  token: \"\"\n"
        "  user:\n");
    cutils_config_t *cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_STRICT), CUTILS_OK);
    assert_int_equal(config_get_int(cfg, "http.port", 0), 9000);
    assert_string_equal(config_get_str(cfg, "security.key_dir"),
                        "/srv/keys # not a comment");
    assert_int_equal(config_key_line(cfg, "http.port"), 8);
    assert_int_equal(config_key_line(cfg, "http.nope"), 0);
    assert_int_equal(config_key_line(NULL, "http.port"), 0);
    config_free(cfg);
}

static void test_validate_typo_suggests_key(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nsecurity:\n  key_dri: /x\n",
                        TEST_CFG ":4: unknown key 'security.key_dri' "
                        "(did you mean 'security.key_dir'?)");
}

static void test_validate_unknown_section(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nbogus:\n  thing: 1\n",
                        TEST_CFG ":4: unknown key 'bogus.thing'");
}

static void test_validate_unknown_top_level_scalar(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nverbose: true\n",
                        TEST_CFG ":3: 'verbose: true' at the top level");
}

static void test_validate_flow_collection(void **state)
{
    (void)state;
    assert_strict_error("log: [unclosed\ndb:\n  path: test.db\n",
                        TEST_CFG ":1: flow collections");
}

static void test_validate_bad_int(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nhttp:\n  port: eighty\n",
                        TEST_CFG ":4: http.port: 'eighty' is not an integer");
    assert_strict_error("db:\n  path: test.db\nhttp:\n  port: 99999999999\n",
                        "is out of range");
    assert_strict_error("db:\n  path: test.db\nhttp:\n  port: 80x\n",
                        "is not an integer");
}

static void test_validate_bad_bool(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nhttp:\n  secure: maybe\n",
                        "http.secure: 'maybe' is not a boolean");
}

static void test_validate_db_key_in_file(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nui:\n  theme: light\n",
                        "ui.theme is kept in the database");

    /* Also when the DB key list arrives separately, as from AppGuard. */
    static const config_key_t only_file[] = {
        { "http.port", CFG_INT, "1", "Port", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    static const config_key_t only_db[] = {
        { "ui.theme", CFG_STRING, "dark", "Theme", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    write_file(TEST_CFG, "db:\n  path: test.db\nui:\n  theme: light\n");
    config_init_opts_t o = { .validate = CFG_VALIDATE_STRICT, .db_keys = only_db };
    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init_opts(&cfg, "testapp", TEST_CFG,
                                      CFG_FIRST_RUN_CONTINUE, only_file, NULL, &o),
                     CUTILS_ERR_CONFIG);
    assert_non_null(strstr(cutils_get_error(), "kept in the database"));
}

static void test_validate_counts_every_problem(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nhttp:\n  port: x\n  secure: y\n"
                        "  nope: 1\n",
                        TEST_CFG ":4: http.port: 'x' is not an integer "
                        "(and 2 more, printed above)");
}

static void test_validate_env_override(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: test.db\n");
    setenv("TESTAPP_HTTP_PORT", "lots", 1);
    cutils_config_t *cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_STRICT), CUTILS_ERR_CONFIG);
    unsetenv("TESTAPP_HTTP_PORT");
    assert_non_null(strstr(cutils_get_error(),
                           "environment TESTAPP_HTTP_PORT (http.port): 'lots' "
                           "is not an integer"));
}

/* Warn reports and continues; off and NULL opts load as 1.3 did. */
static void test_validate_warn_and_off(void **state)
{
    (void)state;
    const char *yaml = "db:\n  path: test.db\nhttp:\n  port: eighty\n"
                       "  prot: 1\n  nested:\n    deep: 1\n";
    config_validate_t modes[] = { CFG_VALIDATE_WARN, CFG_VALIDATE_OFF };
    for (size_t i = 0; i < 2; i++) {
        write_file(TEST_CFG, yaml);
        cutils_config_t *cfg = NULL;
        assert_int_equal(init_v(&cfg, modes[i]), CUTILS_OK);
        assert_int_equal(config_get_int(cfg, "http.port", 7), 7);
        config_free(cfg);
    }
    write_file(TEST_CFG, yaml);
    cutils_config_t *cfg = NULL;
    assert_int_equal(config_init_opts(&cfg, "testapp", TEST_CFG,
                                      CFG_FIRST_RUN_CONTINUE, v_keys, NULL, NULL),
                     CUTILS_OK);
    config_free(cfg);
}

/* A validated init still reports a missing required key as before. */
static void test_validate_then_required(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path:\n");
    cutils_config_t *cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_STRICT), CUTILS_ERR_CONFIG);
    assert_non_null(strstr(cutils_get_error(), "required config key 'db.path'"));
}

/* A generated template validates cleanly, or first run would refuse. */
static void test_validate_generated_template(void **state)
{
    (void)state;
    unlink(TEST_CFG);
    cutils_config_t *cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_STRICT), CUTILS_OK);
    config_free(cfg);
}

/* --- 1.5.0 --- */

/* log.level is checked in the same pass, so it is reported beside the
 * file's other problems instead of dropped when one of them fails init. */
static void test_validate_log_level_joins_the_list(void **state)
{
    (void)state;
    assert_strict_error("db:\n  path: test.db\nhttp:\n  port: abc\n"
                        "log:\n  level: verbose\n",
                        TEST_CFG ":4: http.port: 'abc' is not an integer "
                        "(and 1 more, printed above)");
    assert_strict_error("db:\n  path: test.db\nlog:\n  level: verbose\n",
                        TEST_CFG ":4: log.level: 'verbose' is not one of debug");
}

/* A missing required key is one more problem in the list. */
static void test_validate_required_joins_the_list(void **state)
{
    (void)state;
    assert_strict_error("http:\n  port: abc\n",
                        "(and 1 more, printed above)");
    assert_strict_error("http:\n  port: 1\n",
                        "required config key 'db.path' is missing or empty; "
                        "it has no default");
}

/* The environment satisfies a required key, as it does every read. */
static void test_required_satisfied_by_env(void **state)
{
    (void)state;
    write_file(TEST_CFG, "http:\n  port: 1\n");
    setenv("TESTAPP_DB_PATH", "env.db", 1);
    cutils_config_t *cfg = NULL;
    int rc = init_v(&cfg, CFG_VALIDATE_STRICT);
    unsetenv("TESTAPP_DB_PATH");
    assert_int_equal(rc, CUTILS_OK);
    assert_string_equal(config_get_str(cfg, "db.path"), "env.db");
    config_free(cfg);

    /* OFF mode takes the post-validation check, which names the file. */
    cfg = NULL;
    assert_int_equal(init_v(&cfg, CFG_VALIDATE_OFF), CUTILS_ERR_CONFIG);
    assert_non_null(strstr(cutils_get_error(),
                           TEST_CFG ": required config key 'db.path'"));
}

static const char *const themes[] = { "dark", "light", NULL };

static void app_check(const cutils_config_t *cfg, config_check_t *chk, void *ctx)
{
    (void)cfg;
    int *calls = ctx;
    (*calls)++;
    config_check_int_range(chk, "http.port", 1, 65535);
    config_check_one_of(chk, "security.key_dir", themes);
    if (config_get_bool(cfg, "http.secure", 0))
        config_check_fail(chk, "http.secure", "needs %s", "TLS");
}

static int init_checked(cutils_config_t **cfg, config_validate_t mode,
                        int *calls)
{
    config_init_opts_t o = { .validate = mode, .check = app_check,
                             .check_ctx = calls };
    return config_init_opts(cfg, "testapp", TEST_CFG, CFG_FIRST_RUN_CONTINUE,
                            v_keys, NULL, &o);
}

static void assert_checked_error(const char *yaml, const char *needle)
{
    write_file(TEST_CFG, yaml);
    cutils_config_t *cfg = NULL;
    int calls = 0;
    assert_int_equal(init_checked(&cfg, CFG_VALIDATE_STRICT, &calls),
                     CUTILS_ERR_CONFIG);
    assert_int_equal(calls, 1);
    const char *err = cutils_get_error();
    if (!strstr(err, needle))
        fail_msg("error '%s' lacks '%s'", err, needle);
}

static void test_check_reports_in_file_order(void **state)
{
    (void)state;
    assert_checked_error("db:\n  path: t.db\nsecurity:\n  key_dir: dark\nhttp:\n  port: 99999\n",
                         TEST_CFG ":6: http.port: '99999' is not between 1 "
                         "and 65535");
    assert_checked_error("db:\n  path: t.db\nsecurity:\n  key_dir: dark\nhttp:\n  port: 0\n",
                         "'0' is not between");
    /* Sorted with the built-in problems by line. */
    assert_checked_error("db:\n  path: t.db\nsecurity:\n  key_dir: dark\nhttp:\n  port: -1\n  prot: 1\n",
                         TEST_CFG ":6: http.port: '-1' is not between 1 and "
                         "65535 (and 1 more, printed above)");
    assert_checked_error("db:\n  path: t.db\nsecurity:\n  key_dir: blue\n",
                         "security.key_dir: 'blue' is not one of dark, light");
    assert_checked_error("db:\n  path: t.db\nsecurity:\n  key_dir: dark\nhttp:\n  secure: yes\n",
                         TEST_CFG ":6: http.secure: needs TLS");
}

static void test_check_env_and_default(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: t.db\nsecurity:\n  key_dir: dark\n");
    setenv("TESTAPP_HTTP_PORT", "70000", 1);
    cutils_config_t *cfg = NULL;
    int calls = 0;
    int rc = init_checked(&cfg, CFG_VALIDATE_STRICT, &calls);
    unsetenv("TESTAPP_HTTP_PORT");
    assert_int_equal(rc, CUTILS_ERR_CONFIG);
    assert_non_null(strstr(cutils_get_error(),
        "environment TESTAPP_HTTP_PORT (http.port): '70000' is not between"));

    /* At its default the key has no line and no variable to name. The
     * default /etc/keys is not a theme. */
    write_file(TEST_CFG, "db:\n  path: t.db\n");
    cfg = NULL;
    assert_int_equal(init_checked(&cfg, CFG_VALIDATE_STRICT, &calls),
                     CUTILS_ERR_CONFIG);
    assert_non_null(strstr(cutils_get_error(),
        "security.key_dir: '/etc/keys' is not one of"));
}

/* A non-integer is the type check's to report, once. Clean files pass,
 * WARN loads, OFF does not call the check at all. */
static void test_check_modes(void **state)
{
    (void)state;
    assert_checked_error("db:\n  path: t.db\nhttp:\n  port: abc\n"
                         "security:\n  key_dir: dark\n",
                         TEST_CFG ":4: http.port: 'abc' is not an integer");
    const char *err = cutils_get_error();
    assert_null(strstr(err, "more"));

    write_file(TEST_CFG, "db:\n  path: t.db\nhttp:\n  port: 8080\n"
                         "security:\n  key_dir: light\n");
    cutils_config_t *cfg = NULL;
    int calls = 0;
    assert_int_equal(init_checked(&cfg, CFG_VALIDATE_STRICT, &calls), CUTILS_OK);
    config_free(cfg);

    write_file(TEST_CFG, "db:\n  path: t.db\nhttp:\n  port: 0\n");
    cfg = NULL;
    assert_int_equal(init_checked(&cfg, CFG_VALIDATE_WARN, &calls), CUTILS_OK);
    config_free(cfg);

    calls = 0;
    cfg = NULL;
    assert_int_equal(init_checked(&cfg, CFG_VALIDATE_OFF, &calls), CUTILS_OK);
    assert_int_equal(calls, 0);
    config_free(cfg);

    /* NULL arguments are ignored rather than crashing a check. */
    config_check_fail(NULL, "k", "x");
    config_check_int_range(NULL, "k", 0, 1);
    config_check_one_of(NULL, "k", themes);
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
        cmocka_unit_test_teardown(test_get_env_override, teardown_env),
        cmocka_unit_test_teardown(test_env_not_observed_after_init, teardown_env),
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
        cmocka_unit_test_teardown(test_env_prefix_special_chars, teardown_env),
        cmocka_unit_test_teardown(test_duplicate_section, teardown),
        cmocka_unit_test_teardown(test_db_key_types, teardown),
        cmocka_unit_test_teardown(test_get_db_str_stable_across_reads, teardown),
        cmocka_unit_test_teardown(test_init_env_config_path, teardown_envpath),
        cmocka_unit_test_teardown(test_concurrent_cfg_db_readers_writers, teardown),
        cmocka_unit_test_teardown(test_validate_clean_file_passes, teardown),
        cmocka_unit_test_teardown(test_validate_typo_suggests_key, teardown),
        cmocka_unit_test_teardown(test_validate_unknown_section, teardown),
        cmocka_unit_test_teardown(test_validate_unknown_top_level_scalar, teardown),
        cmocka_unit_test_teardown(test_validate_flow_collection, teardown),
        cmocka_unit_test_teardown(test_validate_bad_int, teardown),
        cmocka_unit_test_teardown(test_validate_bad_bool, teardown),
        cmocka_unit_test_teardown(test_validate_db_key_in_file, teardown),
        cmocka_unit_test_teardown(test_validate_counts_every_problem, teardown),
        cmocka_unit_test_teardown(test_validate_env_override, teardown),
        cmocka_unit_test_teardown(test_validate_warn_and_off, teardown),
        cmocka_unit_test_teardown(test_validate_then_required, teardown),
        cmocka_unit_test_teardown(test_validate_generated_template, teardown),
        cmocka_unit_test_teardown(test_validate_log_level_joins_the_list, teardown),
        cmocka_unit_test_teardown(test_validate_required_joins_the_list, teardown),
        cmocka_unit_test_teardown(test_required_satisfied_by_env, teardown),
        cmocka_unit_test_teardown(test_check_reports_in_file_order, teardown),
        cmocka_unit_test_teardown(test_check_env_and_default, teardown),
        cmocka_unit_test_teardown(test_check_modes, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
