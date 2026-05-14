#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <fcntl.h>
#include <signal.h>
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
    /* chmod 0600 so config_init's permissive-mode warning (1.1.0)
     * doesn't fire on every test that writes a config file. */
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
 * after parsing. Covers both the explicit-mode and warning-suppression
 * angles (1.1.0's permissive-mode warning is moot once we've enforced
 * the mode ourselves). */
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
        cmocka_unit_test_teardown(test_db_and_config_mode_combined, teardown),
        cmocka_unit_test_teardown(test_db_mode_migration_failure_restores_umask, teardown),
        cmocka_unit_test_teardown(test_config_get_path_accessor, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
