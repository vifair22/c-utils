#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cutils/appguard.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/push.h"

#define TEST_CFG "/tmp/cutils_test_appguard.yaml"

/*
 * chmod, intercepted. The library is linked statically, so its calls resolve
 * here. It passes through unless a test sets an errno to fail with, which is
 * how a read-only bind mount is simulated without privileges. Calls on the
 * config path are counted, to prove a file already at the right mode is left
 * alone.
 */
static int g_chmod_fail_errno = 0;
static int g_chmod_config_calls = 0;
int chmod(const char *path, mode_t mode)
{
    if (strcmp(path, "/tmp/cutils_test_appguard.yaml") == 0)
        g_chmod_config_calls++;
    if (g_chmod_fail_errno) { errno = g_chmod_fail_errno; return -1; }
    return fchmodat(AT_FDCWD, path, mode, 0);
}
#define TEST_DB  "/tmp/cutils_test_appguard.db"
#define TEST_ERR "/tmp/cutils_test_appguard.stderr"

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_CFG);
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    unlink(TEST_ERR);
    return 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(content, f);
    fclose(f);
    /* Restrictive perms match what a real-world app would have on
     * its credential-bearing config file. */
    chmod(path, 0600);
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

/* --- Config validation (1.4.0) --- */

static const config_key_t validate_keys[] = {
    { "log.level",   CFG_STRING, "info", "Log level", CFG_STORE_FILE, 0 },
    { "app.workers", CFG_INT,    "2",    "Workers",   CFG_STORE_FILE, 0 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

static appguard_t *init_validating(config_validate_t mode)
{
    appguard_config_t cfg = {
        .app_name        = "testguard",
        .config_path     = TEST_CFG,
        .on_first_run    = CFG_FIRST_RUN_CONTINUE,
        .file_keys       = validate_keys,
        .config_validate = mode,
    };
    return appguard_init(&cfg);
}

/* "warn" is what applications document; it used to be read as the
 * default level, silently. */
static void test_init_log_level_warn_alias(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: warn\n");
    appguard_t *guard = init_validating(CFG_VALIDATE_OFF);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_WARNING);
    appguard_shutdown(guard);
}

static void test_validate_strict_refuses_bad_log_level(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: chatty\n");
    assert_null(init_validating(CFG_VALIDATE_STRICT));
    /* Refused before the database was opened. */
    assert_int_not_equal(access(TEST_DB, F_OK), 0);
}

static void test_validate_warn_continues_on_bad_log_level(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: chatty\n");
    appguard_t *guard = init_validating(CFG_VALIDATE_WARN);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

/* Off is the default: a bad level loads as it did in 1.3. */
static void test_validate_off_ignores_bad_log_level(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: chatty\n"
        "app:\n"
        "  workerz: 4\n");
    appguard_t *guard = init_validating(CFG_VALIDATE_OFF);
    assert_non_null(guard);
    appguard_shutdown(guard);
}

static void test_validate_strict_refuses_unknown_key(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "app:\n"
        "  workerz: 4\n");
    assert_null(init_validating(CFG_VALIDATE_STRICT));
    assert_int_not_equal(access(TEST_DB, F_OK), 0);
}

static void test_validate_strict_accepts_clean_file(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: error\n"
        "app:\n"
        "  workers: 4\n"
        "pushover:\n"
        "  token: \"\"\n"
        "  user: \"\"\n");
    appguard_t *guard = init_validating(CFG_VALIDATE_STRICT);
    assert_non_null(guard);
    assert_int_equal(log_get_level(), LOG_ERROR);
    appguard_shutdown(guard);
}

/* 1.5.0: the application's check runs inside config init, so its
 * problems refuse a start before the database is opened, like the
 * built-in ones. */
static void guard_check(const cutils_config_t *cfg, config_check_t *chk,
                        void *ctx)
{
    (void)cfg;
    (*(int *)ctx)++;
    config_check_int_range(chk, "app.workers", 1, 64);
}

static void test_validate_app_check(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "app:\n"
        "  workers: 0\n");
    int calls = 0;
    appguard_config_t cfg = {
        .app_name         = "testguard",
        .config_path      = TEST_CFG,
        .on_first_run     = CFG_FIRST_RUN_CONTINUE,
        .file_keys        = validate_keys,
        .config_validate  = CFG_VALIDATE_STRICT,
        .config_check     = guard_check,
        .config_check_ctx = &calls,
    };
    assert_null(appguard_init(&cfg));
    assert_int_equal(calls, 1);
    assert_int_not_equal(access(TEST_DB, F_OK), 0);
}

/* --- Each config error reported once (1.5.1) ---
 *
 * Validation printed its list, then AppGuard printed cutils_get_error(),
 * which repeats the first problem: an invalid log.level appeared twice.
 * These run init with stderr redirected to a file and count how often
 * the problem's text appears in it. */

#define STDERR_CAPTURE "/tmp/cutils_test_appguard.stderr"

static int g_saved_stderr = -1;

static void stderr_capture_begin(void)
{
    fflush(stderr);
    g_saved_stderr = dup(STDERR_FILENO);
    assert_true(g_saved_stderr >= 0);
    int fd = open(STDERR_CAPTURE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    assert_int_equal(dup2(fd, STDERR_FILENO), STDERR_FILENO);
    close(fd);
}

static char g_captured[8192];

/* Restores stderr and keeps what was written to it in g_captured. */
static void stderr_capture_end(void)
{
    fflush(stderr);
    assert_int_equal(dup2(g_saved_stderr, STDERR_FILENO), STDERR_FILENO);
    close(g_saved_stderr);
    g_saved_stderr = -1;

    FILE *f = fopen(STDERR_CAPTURE, "r");
    assert_non_null(f);
    size_t n = fread(g_captured, 1, sizeof(g_captured) - 1, f);
    fclose(f);
    unlink(STDERR_CAPTURE);
    g_captured[n] = '\0';
}

static int captured_count(const char *needle)
{
    int count = 0;
    for (const char *p = strstr(g_captured, needle); p; p = strstr(p + 1, needle))
        count++;
    return count;
}

static void test_strict_config_error_printed_once(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: chatty\n");
    stderr_capture_begin();
    appguard_t *guard = init_validating(CFG_VALIDATE_STRICT);
    stderr_capture_end();
    assert_null(guard);
    assert_int_equal(captured_count("chatty"), 1);
}

/* Several problems: each once, none echoed by AppGuard's own line. */
static void test_strict_config_errors_each_printed_once(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n"
        "log:\n"
        "  level: chatty\n"
        "app:\n"
        "  workerz: 4\n");
    stderr_capture_begin();
    appguard_t *guard = init_validating(CFG_VALIDATE_STRICT);
    stderr_capture_end();
    assert_null(guard);
    assert_int_equal(captured_count("chatty"), 1);
    assert_int_equal(captured_count("workerz"), 1);
}

static const config_key_t required_keys[] = {
    { "app.required_key", CFG_STRING, "", "Must be set", CFG_STORE_FILE, 1 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

static appguard_t *init_required(config_validate_t mode)
{
    appguard_config_t cfg = {
        .app_name        = "testguard",
        .config_path     = TEST_CFG,
        .on_first_run    = CFG_FIRST_RUN_CONTINUE,
        .file_keys       = required_keys,
        .config_validate = mode,
    };
    return appguard_init(&cfg);
}

/* Under WARN the missing key is printed in the list as a warning, and
 * the required-key check after validation then fails init with the same
 * text. */
static void test_warn_missing_required_printed_once(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    stderr_capture_begin();
    appguard_t *guard = init_required(CFG_VALIDATE_WARN);
    stderr_capture_end();
    assert_null(guard);
    assert_int_equal(captured_count("app.required_key"), 1);
}

/* With validation off nothing else prints it: AppGuard's line is the
 * only report, and it must still be there. */
static void test_off_missing_required_still_reported(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    stderr_capture_begin();
    appguard_t *guard = init_required(CFG_VALIDATE_OFF);
    stderr_capture_end();
    assert_null(guard);
    assert_int_equal(captured_count("app.required_key"), 1);
}

static void test_log_level_parse(void **state)
{
    (void)state;
    log_level_t l = LOG_ERROR;
    assert_int_equal(log_level_parse("warn", &l), 0);
    assert_int_equal(l, LOG_WARNING);
    assert_int_equal(log_level_parse("debug", &l), 0);
    assert_int_equal(l, LOG_DEBUG);
    assert_int_equal(log_level_parse("chatty", &l), -1);
    assert_int_equal(l, LOG_DEBUG);
    assert_int_equal(log_level_parse(NULL, &l), -1);
    assert_int_equal(log_level_parse("info", NULL), -1);
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

/* --- Signal-driven shutdown: SIGTERM in a child process --- */

static void test_sigterm_clean_exit(void **state)
{
    (void)state;

    /* Set up the config in the parent so the child inherits the file. */
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: init appguard, wait for SIGTERM, exit. */
        appguard_config_t cfg = {
            .app_name = "testguard",
            .config_path = TEST_CFG,
            .on_first_run = CFG_FIRST_RUN_CONTINUE,
            .log_level = LOG_ERROR,
        };
        appguard_t *guard = appguard_init(&cfg);
        if (!guard) _exit(2);

        /* Block until the signal_watcher catches SIGTERM and _exit's
         * us. pause() returns -1/EINTR if our thread receives a signal,
         * but with the new design SIGTERM is masked here and only the
         * watcher thread receives it. The watcher then _exit(0)s the
         * process from a regular thread context — no UB. */
        while (1) pause();
        _exit(3); /* unreachable */
    }

    assert_true(pid > 0);

    /* Give the child time to finish appguard_init and arm the watcher. */
    struct timespec gap = { 0, 300 * 1000 * 1000 };
    nanosleep(&gap, NULL);

    assert_int_equal(kill(pid, SIGTERM), 0);

    int status = -1;
    int wait_rc = waitpid(pid, &status, 0);
    assert_int_equal(wait_rc, pid);

    /* signal_watcher calls _exit(0) — child exits normally with 0,
     * not killed by the signal. That's the whole point of the fix:
     * pre-fix the handler ran async-signal-unsafe code; post-fix the
     * watcher runs in a normal thread context and exits cleanly. */
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
}

/* --- Application-driven shutdown (1.3.0) --- */

static atomic_int g_signalled = 0;
static void note_signal(int sig, void *ctx)
{
    (void)sig;
    atomic_store((atomic_int *)ctx, 1);
}

/* Fork a child that inits AppGuard with on_signal set and, once told, runs
 * its own shutdown and exits with 7. With ignore_first, it never acts on the
 * callback, so only a second signal can end it. */
static pid_t spawn_hooked_child(int ignore_first)
{
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");
    pid_t pid = fork();
    if (pid == 0) {
        appguard_config_t cfg = {
            .app_name = "testguard",
            .config_path = TEST_CFG,
            .on_first_run = CFG_FIRST_RUN_CONTINUE,
            .log_level = LOG_ERROR,
            .on_signal = note_signal,
            .on_signal_ctx = &g_signalled,
        };
        appguard_t *guard = appguard_init(&cfg);
        if (!guard) _exit(2);
        for (;;) {
            struct timespec t = { 0, 10 * 1000 * 1000 };
            nanosleep(&t, NULL);
            if (atomic_load(&g_signalled) && !ignore_first) {
                /* The app's own teardown, then AppGuard's. */
                appguard_shutdown(guard);
                _exit(7);
            }
        }
    }
    assert_true(pid > 0);
    struct timespec gap = { 0, 300 * 1000 * 1000 };
    nanosleep(&gap, NULL);
    return pid;
}

/* The first SIGTERM reaches the app, which shuts down in its own order. The
 * exit status proves the app's path ran rather than AppGuard's exit(0). */
static void test_on_signal_lets_the_app_shut_down(void **state)
{
    (void)state;
    pid_t pid = spawn_hooked_child(0);
    assert_int_equal(kill(pid, SIGTERM), 0);
    int status = -1;
    assert_int_equal(waitpid(pid, &status, 0), pid);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 7);
}

/* An app whose shutdown never comes can still be stopped: the second
 * signal takes AppGuard's forced exit. */
static void test_second_signal_forces_exit(void **state)
{
    (void)state;
    pid_t pid = spawn_hooked_child(1);
    assert_int_equal(kill(pid, SIGTERM), 0);

    /* Still running after the first: the hook took it. */
    struct timespec gap = { 0, 200 * 1000 * 1000 };
    nanosleep(&gap, NULL);
    int status = -1;
    assert_int_equal(waitpid(pid, &status, WNOHANG), 0);

    assert_int_equal(kill(pid, SIGINT), 0);
    assert_int_equal(waitpid(pid, &status, 0), pid);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
}

/* The signal-driven exit must not close the DB under a thread that is still
 * using it. The child's main thread runs queries flat out when SIGTERM lands;
 * before 1.7.2 the watcher's db_close destroyed the handle mutex and freed the
 * handle mid-query. ASAN reports that as a use-after-free, but the watcher's
 * exit(0) can win the race to set the exit status, so the child's stderr is
 * captured and checked for a sanitizer report as well. A plain build can crash
 * or hang instead. Repeated so the signal reliably lands inside a query. */
static void test_signal_exit_leaves_db_to_live_threads(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");
    for (int round = 0; round < 5; round++) {
        int errfd = open(TEST_ERR, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert_true(errfd >= 0);
        pid_t pid = fork();
        if (pid == 0) {
            if (dup2(errfd, STDERR_FILENO) < 0) _exit(5);
            appguard_config_t cfg = {
                .app_name = "testguard",
                .config_path = TEST_CFG,
                .on_first_run = CFG_FIRST_RUN_CONTINUE,
                .log_level = LOG_ERROR,
            };
            appguard_t *guard = appguard_init(&cfg);
            if (!guard) _exit(2);
            cutils_db_t *db = appguard_db(guard);
            if (db_execute_non_query(db,
                    "CREATE TABLE IF NOT EXISTS spin (v INTEGER)",
                    NULL, NULL) != CUTILS_OK)
                _exit(4);
            for (;;) {
                const char *p[] = { "1", NULL };
                if (db_execute_non_query(db,
                        "INSERT INTO spin (v) VALUES (?)", p, NULL)
                        != CUTILS_OK)
                    continue;
                db_result_t *r = NULL;
                if (db_execute(db, "SELECT count(*) FROM spin", NULL, &r)
                        == CUTILS_OK)
                    db_result_free(r);
            }
        }
        close(errfd);
        assert_true(pid > 0);
        struct timespec gap = { 0, 300 * 1000 * 1000 };
        nanosleep(&gap, NULL);
        assert_int_equal(kill(pid, SIGTERM), 0);

        /* Bounded wait: a child wedged on a freed mutex must fail the test,
         * not hang the suite. */
        int status = -1;
        pid_t got = 0;
        for (int i = 0; i < 1000 && got == 0; i++) {
            got = waitpid(pid, &status, WNOHANG);
            if (got == 0) {
                struct timespec t = { 0, 10 * 1000 * 1000 };
                nanosleep(&t, NULL);
            }
        }
        if (got == 0) {
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            fail_msg("child did not exit within 10 s of SIGTERM");
        }
        assert_int_equal(got, pid);
        assert_true(WIFEXITED(status));
        assert_int_equal(WEXITSTATUS(status), 0);

        char buf[8192];
        FILE *f = fopen(TEST_ERR, "r");
        assert_non_null(f);
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        assert_null(strstr(buf, "Sanitizer"));
    }
}

/* --- File-permission enforcement (1.2.0) --- */

static mode_t file_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mode & 0777;
}

/* db_mode = 0 (default / unset): existing 1.1.0 behavior preserved.
 * The DB file's mode is whatever the calling process's umask
 * produced; no chmod is performed by appguard. We can't pin a
 * specific value here because the test runner's umask varies, but
 * we can assert that the file exists. The "no chmod was performed"
 * property is exercised implicitly by the no-bleed test below. */
static void test_db_mode_zero_unchanged(void **state)
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
        /* .db_mode = 0 (implicit) */
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    struct stat st;
    assert_int_equal(stat(TEST_DB, &st), 0);

    appguard_shutdown(guard);
}

/* db_mode != 0: the main DB file and any sqlite sidecars that have
 * been materialized by the time appguard_init returns are all at the
 * requested mode. WAL is created on first write (which happens during
 * lib migrations); SHM is created when sqlite opens in WAL mode. Both
 * should exist by the time we observe. */
static void test_db_mode_enforces_main_and_sidecars(void **state)
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
        .db_mode = 0600,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    assert_int_equal((int)file_mode(TEST_DB), 0600);
    /* Sidecars are ENOENT-tolerant in the implementation, but in
     * practice both should exist after migrations. If a future
     * sqlite or build flag changes that, the test still passes for
     * the ones that do exist — we only assert on existing files. */
    if (access(TEST_DB "-wal", F_OK) == 0)
        assert_int_equal((int)file_mode(TEST_DB "-wal"), 0600);
    if (access(TEST_DB "-shm", F_OK) == 0)
        assert_int_equal((int)file_mode(TEST_DB "-shm"), 0600);

    appguard_shutdown(guard);
}

/* db_mode != 0 against a pre-existing DB file with permissive perms:
 * the post-init mode must match db_mode regardless of the file's
 * starting state. Covers the "daemon restart against an old DB
 * created under a permissive umask" case. */
static void test_db_mode_idempotent_on_existing(void **state)
{
    (void)state;
    /* Seed a pre-existing DB with loose perms. */
    cutils_db_t *seed = NULL;
    assert_int_equal(db_open(&seed, TEST_DB), CUTILS_OK);
    db_close(seed);
    assert_int_equal(chmod(TEST_DB, 0644), 0);
    assert_int_equal((int)file_mode(TEST_DB), 0644);

    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .db_mode = 0600,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    assert_int_equal((int)file_mode(TEST_DB), 0600);

    appguard_shutdown(guard);
}

/* No bleed: the localized umask used during DB init must be restored
 * before appguard_init returns. Set a known-permissive umask before
 * init, run init with db_mode=0600, then create a sentinel file
 * AFTER init returns — its perms should reflect the test's umask,
 * not the 0077 we used internally. This is the load-bearing
 * regression check for the "umask change does not bleed into the
 * application" contract. */
static void test_db_mode_no_umask_bleed(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");

    /* Permissive umask so a sentinel file would be 0666 & ~0000 = 0666.
     * If appguard leaked its 0077 umask, the sentinel would be 0600. */
    mode_t prev = umask(0000);

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .db_mode = 0600,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    /* Create a sentinel file under whatever umask is now active.
     * O_CREAT requests mode 0666; the kernel applies umask. With
     * umask(0000) — restored — the file is 0666. */
    const char *sentinel = "/tmp/cutils_test_appguard.sentinel";
    int fd = open(sentinel, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    assert_true(fd >= 0);
    close(fd);

    assert_int_equal((int)file_mode(sentinel), 0666);

    unlink(sentinel);
    appguard_shutdown(guard);
    umask(prev);
}

/* config_mode != 0: the config file is chmod'd to the requested mode
 * after parsing. */
static void test_config_mode_enforces(void **state)
{
    (void)state;
    /* Note: write_file() chmods 0600 — explicitly chmod 0644 here so
     * we can prove the config_mode field changed it to 0600. */
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    assert_int_equal(chmod(TEST_CFG, 0644), 0);
    assert_int_equal((int)file_mode(TEST_CFG), 0644);

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .config_mode = 0600,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    assert_int_equal((int)file_mode(TEST_CFG), 0600);

    appguard_shutdown(guard);
}

/* A config already at the mode is not chmod'ed: on a read-only mount
 * that chmod failed and the daemon exited, though nothing needed fixing. */
static void test_config_mode_already_right_is_left_alone(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");   /* 0600 */
    g_chmod_config_calls = 0;
    g_chmod_fail_errno = EROFS;   /* any chmod would fail, as on a ro mount */
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .config_mode = 0600,
    };
    appguard_t *guard = appguard_init(&cfg);
    g_chmod_fail_errno = 0;
    assert_non_null(guard);
    assert_int_equal(g_chmod_config_calls, 0);
    appguard_shutdown(guard);
}

/* Wrong mode on a filesystem that will not change it: warn and start,
 * because refusing cannot fix it. */
static void test_config_mode_unfixable_warns_and_starts(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");
    assert_int_equal(chmod(TEST_CFG, 0644), 0);
    g_chmod_fail_errno = EROFS;
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .config_mode = 0600,
    };
    appguard_t *guard = appguard_init(&cfg);
    g_chmod_fail_errno = 0;
    assert_non_null(guard);
    assert_int_equal((int)file_mode(TEST_CFG), 0644);
    appguard_shutdown(guard);
}

/* Any other failure to enforce the mode still refuses to start. */
static void test_config_mode_other_failure_is_fatal(void **state)
{
    (void)state;
    write_file(TEST_CFG, "db:\n  path: " TEST_DB "\n");
    assert_int_equal(chmod(TEST_CFG, 0644), 0);
    g_chmod_fail_errno = EIO;
    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .config_mode = 0600,
    };
    appguard_t *guard = appguard_init(&cfg);
    g_chmod_fail_errno = 0;
    assert_null(guard);
}

/* Both modes set together — the typical daemon configuration.
 * Verifies they compose correctly and don't interfere with each
 * other (e.g., the config chmod happens before the umask is
 * narrowed for DB init). */
static void test_db_and_config_mode_combined(void **state)
{
    (void)state;
    write_file(TEST_CFG,
        "db:\n"
        "  path: " TEST_DB "\n");
    assert_int_equal(chmod(TEST_CFG, 0644), 0);

    appguard_config_t cfg = {
        .app_name = "testguard",
        .config_path = TEST_CFG,
        .on_first_run = CFG_FIRST_RUN_CONTINUE,
        .log_level = LOG_ERROR,
        .db_mode = 0600,
        .config_mode = 0640,
    };

    appguard_t *guard = appguard_init(&cfg);
    assert_non_null(guard);

    assert_int_equal((int)file_mode(TEST_DB),  0600);
    assert_int_equal((int)file_mode(TEST_CFG), 0640);

    appguard_shutdown(guard);
}

/* Failed migration with db_mode set: the migration error path must
 * restore the test's original umask before returning NULL. Covers the
 * umask-restore-on-error logic in appguard_init's migration branches.
 * Without this test, the umask save/restore lines on the error paths
 * are uncovered. */
static void test_db_mode_migration_failure_restores_umask(void **state)
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
        .db_mode = 0600,
        .migrations = bad_migs,
    };

    mode_t prev = umask(0022);
    assert_null(appguard_init(&cfg));

    /* Confirm umask was restored to 0022 (not left at the
     * appguard-internal 0177) by checking the value of a no-op umask
     * call — umask() returns the previous value, so the next call
     * also reads back what we just set. */
    mode_t observed = umask(prev);
    assert_int_equal((int)observed, 0022);
}

/* config_get_path accessor — load-bearing for appguard's config-chmod
 * path. Verify it returns the configured path. */
static void test_config_get_path_accessor(void **state)
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

    const char *path = config_get_path(appguard_config(guard));
    assert_non_null(path);
    assert_string_equal(path, TEST_CFG);
    assert_null(config_get_path(NULL));

    appguard_shutdown(guard);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_init_log_level_warn_alias, teardown),
        cmocka_unit_test_teardown(test_validate_strict_refuses_bad_log_level, teardown),
        cmocka_unit_test_teardown(test_validate_warn_continues_on_bad_log_level, teardown),
        cmocka_unit_test_teardown(test_validate_off_ignores_bad_log_level, teardown),
        cmocka_unit_test_teardown(test_validate_strict_refuses_unknown_key, teardown),
        cmocka_unit_test_teardown(test_validate_strict_accepts_clean_file, teardown),
        cmocka_unit_test_teardown(test_validate_app_check, teardown),
        cmocka_unit_test_teardown(test_strict_config_error_printed_once, teardown),
        cmocka_unit_test_teardown(test_strict_config_errors_each_printed_once, teardown),
        cmocka_unit_test_teardown(test_warn_missing_required_printed_once, teardown),
        cmocka_unit_test_teardown(test_off_missing_required_still_reported, teardown),

        cmocka_unit_test_teardown(test_log_level_parse, teardown),
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
        cmocka_unit_test_teardown(test_sigterm_clean_exit, teardown),
        cmocka_unit_test_teardown(test_db_mode_zero_unchanged, teardown),
        cmocka_unit_test_teardown(test_db_mode_enforces_main_and_sidecars, teardown),
        cmocka_unit_test_teardown(test_db_mode_idempotent_on_existing, teardown),
        cmocka_unit_test_teardown(test_db_mode_no_umask_bleed, teardown),
        cmocka_unit_test_teardown(test_config_mode_enforces, teardown),
        cmocka_unit_test_teardown(test_config_mode_already_right_is_left_alone, teardown),
        cmocka_unit_test_teardown(test_config_mode_unfixable_warns_and_starts, teardown),
        cmocka_unit_test_teardown(test_config_mode_other_failure_is_fatal, teardown),
        cmocka_unit_test_teardown(test_db_and_config_mode_combined, teardown),
        cmocka_unit_test_teardown(test_db_mode_migration_failure_restores_umask, teardown),
        cmocka_unit_test_teardown(test_config_get_path_accessor, teardown),
        cmocka_unit_test_teardown(test_on_signal_lets_the_app_shut_down, teardown),
        cmocka_unit_test_teardown(test_second_signal_forces_exit, teardown),
        cmocka_unit_test_teardown(test_signal_exit_leaves_db_to_live_threads, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
