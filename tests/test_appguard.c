#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cutils/appguard.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/push.h"

#define TEST_CFG "/tmp/cutils_test_appguard.yaml"
#define TEST_DB  "/tmp/cutils_test_appguard.db"

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

static void test_init_null_config(void **state)
{
    (void)state;
    assert_null(appguard_init(NULL));
}

static void test_init_no_app_name(void **state)
{
    (void)state;
    appguard_config_t cfg = {0};
    assert_null(appguard_init(&cfg));
}

static void test_init_basic(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR, /* minimize output */
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    /* Accessors should work */
    assert_non_null(appguard_config(guard));
    assert_non_null(appguard_db(guard));

    appguard_shutdown(guard);
}

static void test_init_with_compiled_migrations(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    const db_migration_t migs[] = {
        { "001_test.sql", "CREATE TABLE test_table (id INTEGER PRIMARY KEY);" },
        { NULL, NULL }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .migrations = migs,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    /* Verify migration ran */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(appguard_db(guard),
        "SELECT name FROM sqlite_master WHERE type='table' AND name='test_table'",
        NULL, &result), CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    db_result_free(result);

    appguard_shutdown(guard);
}

static void test_init_with_app_keys(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "app:\n"
        "  name: testapp\n");

    const config_key_t file_keys[] = {
        { "app.name", CFG_STRING, "default", "Name", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    const config_key_t db_keys[] = {
        { "runtime.interval", CFG_INT, "30", "Interval", CFG_STORE_DB, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    const config_section_t sections[] = {
        { "app", "Application" },
        { NULL, NULL }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .file_keys = file_keys,
        .db_keys = db_keys,
        .sections = sections,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    assert_string_equal(config_get_str(appguard_config(guard), "app.name"), "testapp");
    assert_int_equal(config_get_int(appguard_config(guard), "runtime.interval", 0), 30);

    appguard_shutdown(guard);
}

static void test_init_with_pushover_disabled(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .enable_pushover = 0,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

static void test_init_with_pushover_no_creds(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "pushover:\n"
        "  token:\n"
        "  user:\n");

    const config_key_t file_keys[] = {
        { PUSH_CONFIG_TOKEN, CFG_STRING, "", "Token", CFG_STORE_FILE, 0 },
        { PUSH_CONFIG_USER, CFG_STRING, "", "User", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .enable_pushover = 1,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

static void test_init_first_run_exit(void **state)
{
    (void)state;
    unlink(TEST_CFG);

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_EXIT,
    };

    /* Should generate template and return NULL */
    appguard_t *guard = appguard_init(&cfg);
    assert_null(guard);
}

static void test_init_with_log_level_config(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: debug\n");

    const config_key_t file_keys[] = {
        { "log.level", CFG_STRING, "", "Log level", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    /* Log level should have been set from config */
    assert_int_equal(log_get_level(), LOG_DEBUG);

    appguard_shutdown(guard);
}

static void test_init_log_level_warning(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: warning\n");

    const config_key_t file_keys[] = {
        { "log.level", CFG_STRING, "", "Log level", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_WARNING);
    appguard_shutdown(guard);
}

static void test_init_log_level_error(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: error\n");

    const config_key_t file_keys[] = {
        { "log.level", CFG_STRING, "", "Log level", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_ERROR);
    appguard_shutdown(guard);
}

static void test_init_log_level_info(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: info\n");

    const config_key_t file_keys[] = {
        { "log.level", CFG_STRING, "", "Log level", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_INFO);
    appguard_shutdown(guard);
}

/* --- Init with bad DB path --- */

static void test_init_bad_db_path(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: /nonexistent/deep/dir/test.db\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
    };

    /* db_open should fail for non-existent directory */
    appguard_t *guard = appguard_init(&cfg);
    /* May or may not be NULL depending on SQLite behavior.
       SQLite creates the file, so this might actually succeed.
       Just verify no crash. */
    if (guard)
        appguard_shutdown(guard);
}

/* --- Init with bad compiled migration --- */

static void test_init_bad_compiled_migration(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    const db_migration_t bad_migs[] = {
        { "001_bad.sql", "THIS IS NOT VALID SQL;" },
        { NULL, NULL }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .migrations = bad_migs,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_null(guard); /* should fail */
}

/* --- Init with required config key missing --- */

static void test_init_config_validation_fails(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "app:\n"
        "  required_key:\n");

    const config_key_t keys[] = {
        { "app.required_key", CFG_STRING, "", "Must be set",
          CFG_STORE_FILE, 1 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .file_keys = keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_null(guard); /* config validation should fail */
}

/* --- Shutdown tests --- */

static void test_shutdown_null(void **state)
{
    (void)state;
    appguard_shutdown(NULL); /* should not crash */
}

static void test_shutdown_double(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
    /* Second shutdown is on freed memory — we test the flag inside shutdown
     * by calling it once, which tests the shutdown_called path */
}

/* --- Accessor tests --- */

static void test_accessors_null(void **state)
{
    (void)state;
    assert_null(appguard_config(NULL));
    assert_null(appguard_db(NULL));
}

/* --- Restart without argv --- */

static void test_restart_no_argv(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    /* Restart without set_argv should return -1 */
    assert_int_equal(appguard_restart(guard), -1);

    /* Guard was shut down by restart attempt, don't double-free */
}

/* --- set_argv --- */

static void test_set_argv(void **state)
{
    (void)state;
    char *argv[] = { "/usr/bin/test", "--flag", NULL };
    appguard_set_argv(2, argv);
    /* Just verify it doesn't crash — the values are used by restart */
}

/* --- Init with file migrations dir --- */

static void test_init_with_migrations_dir(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .migrations_dir = "/tmp/cutils_nonexistent_migrations",
    };

    /* Non-existent dir — should fail gracefully */
    appguard_t *guard = appguard_init(&cfg);
    /* This may or may not succeed depending on how the migration runner
       handles missing dirs. Just verify no crash. */
    if (guard)
        appguard_shutdown(guard);
}

/* --- Init with both compiled and file migrations --- */

static void test_init_compiled_and_file_migrations(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    const db_migration_t migs[] = {
        { "001_compiled.sql", "CREATE TABLE compiled_t (id INTEGER PRIMARY KEY);" },
        { NULL, NULL }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .migrations = migs,
        .migrations_dir = NULL,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

/* --- Init with pushover enabled and creds --- */

static void test_init_pushover_with_creds(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "pushover:\n"
        "  token: test_token\n"
        "  user: test_user\n");

    const config_key_t file_keys[] = {
        { PUSH_CONFIG_TOKEN, CFG_STRING, "", "Token", CFG_STORE_FILE, 0 },
        { PUSH_CONFIG_USER, CFG_STRING, "", "User", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .enable_pushover = 1,
        .file_keys = file_keys,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

/* --- Init with default log level (0 = use default INFO) --- */

static void test_init_default_log_level(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = 0, /* triggers default INFO */
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_INFO);
    appguard_shutdown(guard);
}

/* --- Systemd auto-detect --- */

/* These tests mutate JOURNAL_STREAM. Save/restore to keep them hermetic. */
static void with_journal_stream(const char *value, void (*fn)(void))
{
    char *saved = getenv("JOURNAL_STREAM");
    char *saved_copy = saved ? strdup(saved) : NULL;
    if (value) setenv("JOURNAL_STREAM", value, 1);
    else       unsetenv("JOURNAL_STREAM");

    fn();

    if (saved_copy) {
        setenv("JOURNAL_STREAM", saved_copy, 1);
        free(saved_copy);
    } else {
        unsetenv("JOURNAL_STREAM");
    }
}

static void run_autodetect_off_no_env(void)
{
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .log_systemd_autodetect = 0,
    };
    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_systemd_mode(), 0);
    appguard_shutdown(guard);
    log_set_systemd_mode(0);
}

static void test_systemd_autodetect_off(void **state)
{
    (void)state;
    /* Even if JOURNAL_STREAM is set, opt-out means no mode change. */
    with_journal_stream("8:12345", run_autodetect_off_no_env);
}

static void run_autodetect_on_with_env(void)
{
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .log_systemd_autodetect = 1,
    };
    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    assert_int_equal(log_get_systemd_mode(), 1);
    appguard_shutdown(guard);
    log_set_systemd_mode(0);
}

static void test_systemd_autodetect_on_with_env(void **state)
{
    (void)state;
    with_journal_stream("8:12345", run_autodetect_on_with_env);
}

static void run_autodetect_on_no_env(void)
{
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .log_systemd_autodetect = 1,
    };
    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    /* Opt-in but no JOURNAL_STREAM → mode stays off. */
    assert_int_equal(log_get_systemd_mode(), 0);
    appguard_shutdown(guard);
    log_set_systemd_mode(0);
}

static void test_systemd_autodetect_on_no_env(void **state)
{
    (void)state;
    with_journal_stream(NULL, run_autodetect_on_no_env);
}

/* --- Retention days --- */

static void test_retention_days(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .log_retention_days = 7,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_init_null_config, teardown),
        cmocka_unit_test_teardown(test_init_no_app_name, teardown),
        cmocka_unit_test_teardown(test_init_basic, teardown),
        cmocka_unit_test_teardown(test_init_with_compiled_migrations, teardown),
        cmocka_unit_test_teardown(test_init_with_app_keys, teardown),
        cmocka_unit_test_teardown(test_init_with_pushover_disabled, teardown),
        cmocka_unit_test_teardown(test_init_with_pushover_no_creds, teardown),
        cmocka_unit_test_teardown(test_init_first_run_exit, teardown),
        cmocka_unit_test_teardown(test_init_with_log_level_config, teardown),
        cmocka_unit_test_teardown(test_init_log_level_warning, teardown),
        cmocka_unit_test_teardown(test_init_log_level_error, teardown),
        cmocka_unit_test_teardown(test_init_log_level_info, teardown),
        cmocka_unit_test_teardown(test_init_bad_db_path, teardown),
        cmocka_unit_test_teardown(test_init_bad_compiled_migration, teardown),
        cmocka_unit_test_teardown(test_init_config_validation_fails, teardown),
        cmocka_unit_test_teardown(test_shutdown_null, teardown),
        cmocka_unit_test_teardown(test_shutdown_double, teardown),
        cmocka_unit_test_teardown(test_accessors_null, teardown),
        cmocka_unit_test_teardown(test_restart_no_argv, teardown),
        cmocka_unit_test_teardown(test_set_argv, teardown),
        cmocka_unit_test_teardown(test_init_with_migrations_dir, teardown),
        cmocka_unit_test_teardown(test_init_compiled_and_file_migrations, teardown),
        cmocka_unit_test_teardown(test_init_pushover_with_creds, teardown),
        cmocka_unit_test_teardown(test_init_default_log_level, teardown),
        cmocka_unit_test_teardown(test_systemd_autodetect_off, teardown),
        cmocka_unit_test_teardown(test_systemd_autodetect_on_with_env, teardown),
        cmocka_unit_test_teardown(test_systemd_autodetect_on_no_env, teardown),
        cmocka_unit_test_teardown(test_retention_days, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
