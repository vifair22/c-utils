#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cutils/log.h"
#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB "/tmp/cutils_test_log.db"

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

/* --- Init/shutdown without DB --- */

static void test_init_no_db(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);
    log_write(LOG_INFO, "test", "hello %s", "world");
    log_shutdown();
}

static void test_init_shutdown_idempotent(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);
    log_shutdown();
    log_shutdown(); /* second call should be safe */
}

/* --- Level filtering --- */

static void test_level_filtering(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_WARNING, 0), CUTILS_OK);

    /* These should be filtered out (below WARNING) */
    log_write(LOG_DEBUG, "test", "debug msg");
    log_write(LOG_INFO, "test", "info msg");

    /* These should pass through */
    log_write(LOG_WARNING, "test", "warning msg");
    log_write(LOG_ERROR, "test", "error msg");

    log_shutdown();
}

static void test_set_get_level(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    assert_int_equal(log_get_level(), LOG_INFO);

    log_set_level(LOG_DEBUG);
    assert_int_equal(log_get_level(), LOG_DEBUG);

    log_set_level(LOG_ERROR);
    assert_int_equal(log_get_level(), LOG_ERROR);

    log_shutdown();
}

/* --- Stream callbacks --- */

typedef struct {
    int count;
    char last_level[16];
    char last_message[256];
} stream_state_t;

static void test_stream_cb(const char *timestamp, const char *level,
                           const char *func, const char *message,
                           void *userdata)
{
    (void)timestamp;
    (void)func;
    stream_state_t *ss = userdata;
    ss->count++;
    snprintf(ss->last_level, sizeof(ss->last_level), "%s", level);
    snprintf(ss->last_message, sizeof(ss->last_message), "%s", message);
}

static void test_stream_register(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    stream_state_t ss = {0};
    int handle = log_stream_register(test_stream_cb, &ss);
    assert_true(handle >= 0);

    log_write(LOG_INFO, "test", "streamed message");
    assert_int_equal(ss.count, 1);
    assert_string_equal(ss.last_level, "info");
    assert_string_equal(ss.last_message, "streamed message");

    /* Debug should be filtered by min level */
    log_write(LOG_DEBUG, "test", "not streamed");
    assert_int_equal(ss.count, 1);

    log_stream_unregister(handle);

    /* After unregister, callback should not fire */
    log_write(LOG_INFO, "test", "after unregister");
    assert_int_equal(ss.count, 1);

    log_shutdown();
}

static void test_stream_unregister_invalid(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    /* Invalid handles should not crash */
    log_stream_unregister(-1);
    log_stream_unregister(99);

    log_shutdown();
}

static void test_stream_max_slots(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    int handles[8];
    stream_state_t states[8] = {{0}};

    /* Fill all 8 slots */
    for (int i = 0; i < 8; i++) {
        handles[i] = log_stream_register(test_stream_cb, &states[i]);
        assert_true(handles[i] >= 0);
    }

    /* 9th should fail */
    assert_int_equal(log_stream_register(test_stream_cb, NULL), -1);

    /* Fire a message — all 8 should receive it */
    log_write(LOG_INFO, "test", "broadcast");
    for (int i = 0; i < 8; i++)
        assert_int_equal(states[i].count, 1);

    /* Unregister one, register again */
    log_stream_unregister(handles[3]);
    int h = log_stream_register(test_stream_cb, NULL);
    assert_true(h >= 0);

    for (int i = 0; i < 8; i++)
        log_stream_unregister(handles[i]);
    log_stream_unregister(h);

    log_shutdown();
}

/* --- DB persistence --- */

static void test_log_to_db(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(log_init(db, LOG_DEBUG, 0), CUTILS_OK);

    log_write(LOG_INFO, "test_func", "db message one");
    log_write(LOG_ERROR, "test_func", "db message two");
    log_write(LOG_DEBUG, "test_func", "db debug msg");

    log_shutdown();

    /* Verify messages were persisted */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db,
        "SELECT level, function, message FROM logs ORDER BY rowid",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_true(result->nrows >= 2);

    db_result_free(result);
    db_close(db);
}

/* --- All log levels output --- */

static void test_all_levels(void **state)
{
    (void)state;
    assert_int_equal(log_init(NULL, LOG_DEBUG, 0), CUTILS_OK);

    log_write(LOG_DEBUG, "fn", "debug");
    log_write(LOG_INFO, "fn", "info");
    log_write(LOG_WARNING, "fn", "warning");
    log_write(LOG_ERROR, "fn", "error");

    log_shutdown();
}

/* --- All levels through DB path --- */

static void test_all_levels_to_db(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(log_init(db, LOG_DEBUG, 0), CUTILS_OK);

    /* Exercise all level_str/level_color switch branches through enqueue path */
    log_write(LOG_DEBUG, "fn", "debug to db");
    log_write(LOG_INFO, "fn", "info to db");
    log_write(LOG_WARNING, "fn", "warning to db");
    log_write(LOG_ERROR, "fn", "error to db");

    log_shutdown();

    /* Verify all 4 levels persisted */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT DISTINCT level FROM logs ORDER BY level", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_true(result->nrows >= 4);
    db_result_free(result);

    db_close(db);
}

/* --- Init without DB, shutdown idempotent with streams --- */

static void test_streams_with_db(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    assert_int_equal(log_init(db, LOG_INFO, 0), CUTILS_OK);

    stream_state_t ss = {0};
    int h = log_stream_register(test_stream_cb, &ss);
    assert_true(h >= 0);

    log_write(LOG_WARNING, "test", "stream + db");
    assert_int_equal(ss.count, 1);
    assert_string_equal(ss.last_level, "warning");

    log_stream_unregister(h);
    log_shutdown();
    db_close(db);
}

/* --- Console output formatting (color suppression) --- */

/* Capture stdout into a file while running fn(), then read the file back
 * into buf. Used to assert what the console writer actually emits. */
static void capture_console(const char *path, char *buf, size_t buflen,
                            void (*fn)(void))
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    {
        FILE *f = fopen(path, "w");
        assert_non_null(f);
        dup2(fileno(f), STDOUT_FILENO);
        fclose(f);
    }

    fn();
    fflush(stdout);

    dup2(saved, STDOUT_FILENO);
    close(saved);

    FILE *f = fopen(path, "r");
    assert_non_null(f);
    size_t n = fread(buf, 1, buflen - 1, f);
    buf[n] = '\0';
    fclose(f);
    unlink(path);
}

static void emit_one_info_line(void)
{
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);
    log_write(LOG_INFO, "test", "no escapes please");
    log_shutdown();
}

static void test_no_escapes_when_not_tty(void **state)
{
    /* Test env pipes stdout to cmocka's harness, so isatty() is false.
     * Even with NO_COLOR unset, the TTY check alone must suppress escapes. */
    (void)state;
    unsetenv("NO_COLOR");

    char buf[4096];
    capture_console("/tmp/cutils_test_log_notty.txt", buf, sizeof(buf),
                    emit_one_info_line);

    assert_null(strchr(buf, '\033'));
    assert_non_null(strstr(buf, "no escapes please"));
    assert_non_null(strstr(buf, "[test]"));
}

static void test_no_color_env_suppresses_escapes(void **state)
{
    (void)state;
    setenv("NO_COLOR", "1", 1);

    char buf[4096];
    capture_console("/tmp/cutils_test_log_nocolor.txt", buf, sizeof(buf),
                    emit_one_info_line);

    unsetenv("NO_COLOR");

    assert_null(strchr(buf, '\033'));
    assert_non_null(strstr(buf, "no escapes please"));
}

/* --- Systemd mode --- */

static void emit_all_four_levels(void)
{
    assert_int_equal(log_init(NULL, LOG_DEBUG, 0), CUTILS_OK);
    /* log_init resets systemd mode (see test_reinit_resets_systemd_mode),
     * so the set must happen after init for the writer to see it. */
    log_set_systemd_mode(1);
    log_write(LOG_DEBUG,   "fn", "d-msg");
    log_write(LOG_INFO,    "fn", "i-msg");
    log_write(LOG_WARNING, "fn", "w-msg");
    log_write(LOG_ERROR,   "fn", "e-msg");
    log_shutdown();
}

static void test_systemd_mode_format(void **state)
{
    /* Verify the systemd-mode formatter:
     *   - <N> priority prefix per RFC 5424 (7/6/4/3 = debug/info/warning/error)
     *   - no timestamp (no "20" digit prefix that the standard format produces)
     *   - no ANSI escapes
     *   - all four levels, including ERROR, appear in stdout (proving the
     *     stream split was dropped — captured fd is stdout only) */
    (void)state;
    /* Mode is set inside emit_all_four_levels after log_init, since
     * log_init resets the flag. */
    char buf[4096];
    capture_console("/tmp/cutils_test_log_systemd.txt", buf, sizeof(buf),
                    emit_all_four_levels);

    log_set_systemd_mode(0);

    assert_non_null(strstr(buf, "<7>[fn] d-msg"));
    assert_non_null(strstr(buf, "<6>[fn] i-msg"));
    assert_non_null(strstr(buf, "<4>[fn] w-msg"));
    assert_non_null(strstr(buf, "<3>[fn] e-msg"));
    assert_null(strchr(buf, '\033'));
    assert_null(strstr(buf, "20")); /* no year-prefixed timestamp */
}

static void test_systemd_mode_setter(void **state)
{
    (void)state;
    log_set_systemd_mode(0);
    assert_int_equal(log_get_systemd_mode(), 0);
    log_set_systemd_mode(1);
    assert_int_equal(log_get_systemd_mode(), 1);
    log_set_systemd_mode(42); /* nonzero is truthy */
    assert_int_equal(log_get_systemd_mode(), 1);
    log_set_systemd_mode(0);
}

/* --- DB with retention --- */

static void test_log_retention(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);

    /* Insert an old log entry manually */
    const char *params[] = { "2020-01-01 00:00:00", "info", "old", "old msg", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO logs (timestamp, level, function, message) VALUES (?, ?, ?, ?)",
        params, NULL), CUTILS_OK);

    /* Start with retention = 1 day */
    assert_int_equal(log_init(db, LOG_INFO, 1), CUTILS_OK);

    /* Log something to trigger the writer thread which does cleanup */
    log_write(LOG_INFO, "test", "current message");

    log_shutdown();

    /* The old entry should have been cleaned up */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT COUNT(*) FROM logs WHERE timestamp < '2021-01-01'",
               NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_string_equal(result->rows[0][0], "0");
    db_result_free(result);

    db_close(db);
}

/* --- Stream callback re-entry: must not deadlock on the registry lock --- */

static int reentry_cb_calls = 0;

static void reentry_inner_cb(const char *timestamp, const char *level,
                              const char *func, const char *message,
                              void *userdata)
{
    (void)timestamp; (void)level; (void)func; (void)message; (void)userdata;
    reentry_cb_calls++;
}

static void reentry_outer_cb(const char *timestamp, const char *level,
                              const char *func, const char *message,
                              void *userdata)
{
    (void)timestamp; (void)level; (void)func; (void)message; (void)userdata;
    reentry_cb_calls++;
    /* Re-enter log_* from inside a stream callback. Pre-fix, fire_streams
     * held stream_mutex across the callback, so this log_warn would
     * try to lock the same non-recursive mutex and deadlock. With the
     * snapshot-then-fire-unlocked design, this completes normally. */
    static int reentered = 0;
    if (!reentered) {
        reentered = 1;
        log_warn("re-entry from stream callback");
        reentered = 0;
    }
}

static void test_stream_callback_reentry_no_deadlock(void **state)
{
    (void)state;
    reentry_cb_calls = 0;

    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    int h_outer = log_stream_register(reentry_outer_cb, NULL);
    int h_inner = log_stream_register(reentry_inner_cb, NULL);
    assert_true(h_outer >= 0 && h_inner >= 0);

    /* If the lock-during-callback bug were still present this hangs
     * forever and CI eventually times out the runner. */
    log_info("trigger fan-out");

    /* Outer fires twice (once for "trigger fan-out", once for the
     * nested "re-entry..."), inner fires twice on each fan-out, plus
     * any extra. Just assert it ran more than once. */
    assert_true(reentry_cb_calls >= 2);

    log_stream_unregister(h_outer);
    log_stream_unregister(h_inner);
    log_shutdown();
}

/* --- init/shutdown/init cycle must reset module state --- */

static void test_reinit_resets_systemd_mode(void **state)
{
    (void)state;

    /* First cycle: enable systemd mode. */
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);
    log_set_systemd_mode(1);
    assert_int_equal(log_get_systemd_mode(), 1);
    log_shutdown();

    /* Pre-fix: log_systemd_mode survived the cycle and the second init
     * inherited the prior 1. The comment on the declaration claimed
     * the field was cleared on init, but the code didn't actually do
     * it. Now it does. */
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);
    assert_int_equal(log_get_systemd_mode(), 0);
    log_shutdown();
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_init_no_db, teardown),
        cmocka_unit_test_teardown(test_init_shutdown_idempotent, teardown),
        cmocka_unit_test_teardown(test_level_filtering, teardown),
        cmocka_unit_test_teardown(test_set_get_level, teardown),
        cmocka_unit_test_teardown(test_stream_register, teardown),
        cmocka_unit_test_teardown(test_stream_unregister_invalid, teardown),
        cmocka_unit_test_teardown(test_stream_max_slots, teardown),
        cmocka_unit_test_teardown(test_log_to_db, teardown),
        cmocka_unit_test_teardown(test_all_levels, teardown),
        cmocka_unit_test_teardown(test_all_levels_to_db, teardown),
        cmocka_unit_test_teardown(test_streams_with_db, teardown),
        cmocka_unit_test_teardown(test_no_escapes_when_not_tty, teardown),
        cmocka_unit_test_teardown(test_no_color_env_suppresses_escapes, teardown),
        cmocka_unit_test_teardown(test_systemd_mode_format, teardown),
        cmocka_unit_test_teardown(test_systemd_mode_setter, teardown),
        cmocka_unit_test_teardown(test_log_retention, teardown),
        cmocka_unit_test_teardown(test_stream_callback_reentry_no_deadlock, teardown),
        cmocka_unit_test_teardown(test_reinit_resets_systemd_mode, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
