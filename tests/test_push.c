#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <curl/curl.h>

#include "cutils/push.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"

/* Exposed by push.c specifically for this test — see comment on the
 * definition in src/push.c. Builds the URL-encoded POST body into a
 * newly-malloc'd buffer sized exactly to the formatted content. */
extern char *cutils_push_build_postfields(CURL *curl,
                                          const char *token, const char *user,
                                          const char *title, const char *message,
                                          const char *timestamp, const char *ttl,
                                          int html, int priority);

/* Same arrangement: points the worker at a local listener (NULL restores
 * the Pushover endpoint). */
extern void cutils_push_set_url(const char *url);

#define TEST_DB  "/tmp/cutils_test_push.db"
#define TEST_CFG "/tmp/cutils_test_push.yaml"

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    unlink(TEST_CFG);
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

/* Helper: set up a DB + config with pushover keys */
typedef struct {
    cutils_db_t     *db;
    cutils_config_t *cfg;
} push_fixture_t;

static push_fixture_t make_fixture(int with_creds)
{
    push_fixture_t fix = {0};

    if (with_creds) {
        write_file(TEST_CFG,
            "db:\n"
            "  path: test.db\n"
            "pushover:\n"
            "  token: test_token_abc\n"
            "  user: test_user_xyz\n");
    } else {
        write_file(TEST_CFG,
            "db:\n"
            "  path: test.db\n"
            "pushover:\n"
            "  token:\n"
            "  user:\n");
    }

    const config_key_t keys[] = {
        { PUSH_CONFIG_TOKEN, CFG_STRING, "", "Token", CFG_STORE_FILE, 0 },
        { PUSH_CONFIG_USER, CFG_STRING, "", "User", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    const config_section_t sections[] = {
        { "pushover", "Pushover Notifications" },
        { NULL, NULL }
    };

    assert_int_equal(config_init(&fix.cfg, "testapp", TEST_CFG,
                                 CFG_FIRST_RUN_CONTINUE, keys, sections), CUTILS_OK);
    assert_int_equal(db_open(&fix.db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_run_lib_migrations(fix.db), CUTILS_OK);

    /* Need log for push_init */
    assert_int_equal(log_init(NULL, LOG_INFO, 0), CUTILS_OK);

    return fix;
}

static void free_fixture(push_fixture_t *fix)
{
    log_shutdown();
    db_close(fix->db);
    config_free(fix->cfg);
}

/* --- Tests --- */

static void test_push_init_no_creds(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(0);

    /* Without creds, push_init should succeed but disable push */
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    /* push_shutdown is safe even when not running */
    push_shutdown();

    free_fixture(&fix);
}

static void test_push_send_no_op_when_uninitialized(void **state)
{
    (void)state;
    /* Disabled contract: when push_init was never called (e.g.
     * AppGuard's enable_pushover=0), push_send_opts must return
     * CUTILS_OK silently. App code calls push_send unconditionally
     * without gating on a per-call enabled check. */
    push_opts_t opts = {
        .title = "test",
        .message = "test msg",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);
}

static void test_push_send_no_op_when_creds_missing(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(0);

    /* push_init with no creds clears push_db so push_send_opts
     * silently no-ops. Pre-1.0.2 left push_db set, which meant
     * sends inserted rows that nothing drained — a slow queue
     * leak. We verify both: rc is CUTILS_OK and the push table
     * stays empty. */
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "test",
        .message = "msg",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);
    assert_int_equal(push_send("title", "body"), CUTILS_OK);

    db_result_t *r = NULL;
    assert_int_equal(db_execute(fix.db, "SELECT COUNT(*) FROM push",
                                NULL, &r), CUTILS_OK);
    assert_string_equal(r->rows[0][0], "0");
    db_result_free(r);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_enqueue(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);

    /* Init with creds — this starts the worker thread which will try to send
     * and fail (no real Pushover server). That's fine, we just test the enqueue. */
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Send a short message */
    assert_int_equal(push_send("Test Title", "Short message"), CUTILS_OK);

    /* Verify it was stored in the push table */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT title, message FROM push ORDER BY rowid", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_true(result->nrows >= 1);
    assert_string_equal(result->rows[0][0], "Test Title");
    assert_string_equal(result->rows[0][1], "Short message");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_opts_override(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "Custom",
        .message = "With overrides",
        .ttl = 3600,
        .timestamp = 1000000,
        .token = "custom_token",
        .user = "custom_user",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    /* Verify custom token/user were stored */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db, "SELECT token, user, ttl FROM push ORDER BY rowid DESC LIMIT 1",
               NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_string_equal(result->rows[0][0], "custom_token");
    assert_string_equal(result->rows[0][1], "custom_user");
    assert_string_equal(result->rows[0][2], "3600");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_long_message_split(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Build a message > 1024 chars */
    char long_msg[2200];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    /* Add some newlines for split points */
    for (int i = 500; i < 2100; i += 500)
        long_msg[i] = '\n';

    assert_int_equal(push_send("Split Test", long_msg), CUTILS_OK);

    /* Should have been split into multiple rows */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db, "SELECT COUNT(*) FROM push WHERE title LIKE 'Split Test%'",
               NULL, &result), CUTILS_OK);
    assert_non_null(result);
    int parts = atoi(result->rows[0][0]);
    assert_true(parts >= 2);
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_opts_partial_creds(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Token set but user empty */
    push_opts_t opts = {
        .title = "Partial",
        .message = "Only token set",
        .token = "has_token",
        .user = "",
    };
    /* Should fall back to config for user, which has creds */
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    /* User set but token empty */
    push_opts_t opts2 = {
        .title = "Partial2",
        .message = "Only user set",
        .token = "",
        .user = "has_user",
    };
    assert_int_equal(push_send_opts(&opts2), CUTILS_OK);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_html_flag(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "HTML Test",
        .message = "<b>Bold</b> message",
        .html = 1,
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    /* Verify html=1 was stored */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'HTML Test'",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "1");
    assert_string_equal(result->rows[0][1], "0");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_priority(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "High Priority",
        .message = "Urgent alert",
        .priority = PUSH_PRIORITY_HIGH,
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT priority FROM push WHERE title = 'High Priority'",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "1");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_html_and_priority(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "Combined",
        .message = "<font color=\"#ef4444\"><b>CRITICAL</b></font>",
        .html = 1,
        .priority = PUSH_PRIORITY_HIGH,
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'Combined'",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "1");
    assert_string_equal(result->rows[0][1], "1");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_defaults_zero(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* push_send uses zero-initialized opts — html and priority should be 0 */
    assert_int_equal(push_send("Default", "Plain message"), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'Default'",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "0");
    assert_string_equal(result->rows[0][1], "0");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_send_negative_priority(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_opts_t opts = {
        .title = "Silent",
        .message = "No alert",
        .priority = PUSH_PRIORITY_LOWEST,
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT priority FROM push WHERE title = 'Silent'",
        NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_string_equal(result->rows[0][0], "-2");
    db_result_free(result);

    push_shutdown();
    free_fixture(&fix);
}

static void test_push_shutdown_without_init(void **state)
{
    (void)state;
    /* Should not crash */
    push_shutdown();
}

/* --- Post-shutdown call must silently drop, not enqueue --- */

static void test_push_send_after_shutdown_no_op(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);

    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    push_shutdown();

    /* Post-shutdown push_db is NULL — disabled contract says
     * push_send returns CUTILS_OK silently. The row must NOT be
     * inserted (no live worker to drain it) but the caller doesn't
     * need to know push has been torn down. */
    push_opts_t opts = {
        .title = "post-shutdown",
        .message = "should not enqueue",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_OK);
    assert_int_equal(push_send("late", "drop"), CUTILS_OK);

    db_result_t *r = NULL;
    assert_int_equal(db_execute(fix.db, "SELECT COUNT(*) FROM push",
                                NULL, &r), CUTILS_OK);
    assert_string_equal(r->rows[0][0], "0");
    db_result_free(r);

    free_fixture(&fix);
}

/* --- Concurrent multi-part sends must not interleave chunks --- */

typedef struct {
    char        marker;   /* 'A' or 'B' */
    push_opts_t opts;
} chunk_thread_arg_t;

static void *chunk_sender(void *p)
{
    chunk_thread_arg_t *a = p;
    assert_int_equal(push_send_opts(&a->opts), CUTILS_OK);
    return NULL;
}

static void test_concurrent_push_send_multi_part_atomic(void **state)
{
    (void)state;
    /* Creds fixture: bogus token/user so the worker's send attempts
     * get 4xx (SEND_PERMANENT_FAIL) and rows are marked failed=1
     * instead of deleted. We can then walk the queue post-drain and
     * verify rowid ordering. Pre-1.0.2 this test ran with a no-creds
     * fixture (push_db left set, worker never started) — that's no
     * longer possible because no-creds now nulls push_db and
     * push_send becomes a silent no-op. */
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    char msg_a[2500];
    char msg_b[2500];
    memset(msg_a, 'A', sizeof(msg_a) - 1);
    memset(msg_b, 'B', sizeof(msg_b) - 1);
    msg_a[sizeof(msg_a) - 1] = '\0';
    msg_b[sizeof(msg_b) - 1] = '\0';
    for (int i = 800; i < 2400; i += 800) {
        msg_a[i] = '\n';
        msg_b[i] = '\n';
    }

    chunk_thread_arg_t arg_a = {
        .marker = 'A',
        .opts = { .title = "A", .message = msg_a,
                  .token = "tok", .user = "usr" },
    };
    chunk_thread_arg_t arg_b = {
        .marker = 'B',
        .opts = { .title = "B", .message = msg_b,
                  .token = "tok", .user = "usr" },
    };

    pthread_t tA, tB;
    pthread_create(&tA, NULL, chunk_sender, &arg_a);
    pthread_create(&tB, NULL, chunk_sender, &arg_b);
    pthread_join(tA, NULL);
    pthread_join(tB, NULL);

    /* Drain via shutdown: the worker tries each row, gets 4xx from
     * the bogus creds, marks failed=1. Rows persist in rowid order. */
    push_shutdown();

    db_result_t *r = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT title FROM push ORDER BY rowid", NULL, &r), CUTILS_OK);
    assert_non_null(r);
    assert_true(r->nrows >= 6);  /* 3+ parts each */

    char first = r->rows[0][0][0];
    int  switched = 0;
    for (int i = 1; i < r->nrows; i++) {
        char c = r->rows[i][0][0];
        if (c != first) {
            switched = 1;
        } else if (switched) {
            db_result_free(r);
            free_fixture(&fix);
            fail_msg("multi-part chunks interleaved across concurrent senders");
        }
    }

    db_result_free(r);
    free_fixture(&fix);
}

/* --- push_init must drain rows left from a previous run --- */

static _Atomic int drain_signal_seen = 0;

static void drain_log_cb(const char *ts, const char *level, const char *func,
                         const char *msg, void *userdata)
{
    (void)ts; (void)level; (void)msg; (void)userdata;
    /* The worker emits log_warn "push transient failure" or log_error
     * "push permanently failed" after attempting to send. Either log
     * proves it iterated through a row at startup. */
    if (func && strcmp(func, "push_worker_thread") == 0)
        atomic_store(&drain_signal_seen, 1);
}

static void test_push_init_drains_preexisting_rows(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    atomic_store(&drain_signal_seen, 0);

    /* Pre-populate a row directly — simulates a row left over from a
     * previous run that crashed before delivery. */
    const char *params[] = {
        "12345", "tok", "usr", "86400",
        "preexisting", "leftover", "0", "0", "0", NULL
    };
    assert_int_equal(db_execute_non_query(fix.db,
        "INSERT INTO push (timestamp, token, user, ttl, message, title, "
        "failed, html, priority) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL), CUTILS_OK);

    int h = log_stream_register(drain_log_cb, NULL);
    assert_true(h >= 0);

    /* push_init starts the worker. Pre-fix, push_notify was left at
     * 0, so the worker hit cond_wait immediately and never noticed
     * the pre-existing row. Post-fix, push_notify is 1 at init, so
     * the worker's first iteration drains. */
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Poll up to 30s for the worker to attempt delivery. Network
     * round-trip to api.pushover.net is well under that ceiling for
     * a 4xx; even a transient failure path emits a log_warn that
     * trips the signal. Pre-fix: signal never raised, test fails. */
    int got = 0;
    for (int i = 0; i < 300; i++) {
        if (atomic_load(&drain_signal_seen)) { got = 1; break; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert_true(got);

    log_stream_unregister(h);
    push_shutdown();
    free_fixture(&fix);
}

/* --- TTL give-up: rows past timestamp+ttl get marked failed=1
 *     without ever calling send_one. Tests the new
 *     "stop retrying once the message has gone stale" behavior. --- */

static _Atomic int ttl_expired_signal = 0;

static void ttl_expired_log_cb(const char *ts, const char *level,
                               const char *func, const char *msg, void *userdata)
{
    (void)ts; (void)level; (void)userdata;
    if (func && strcmp(func, "push_worker_thread") == 0 &&
        msg && strstr(msg, "push expired before delivery"))
        atomic_store(&ttl_expired_signal, 1);
}

static void test_ttl_expired_row_marked_failed(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    atomic_store(&ttl_expired_signal, 0);

    /* Wildly-expired row: timestamp=1 (1970), ttl=1. Worker must
     * mark it failed=1 from the TTL check, no network call. */
    const char *params[] = {
        "1", "tok", "usr", "1",
        "stale body", "stale title",
        "0", "0", "0", "0", "0", NULL
    };
    assert_int_equal(db_execute_non_query(fix.db,
        "INSERT INTO push (timestamp, token, user, ttl, message, title, "
        "failed, html, priority, attempts, next_retry_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL), CUTILS_OK);

    int h = log_stream_register(ttl_expired_log_cb, NULL);
    assert_true(h >= 0);

    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    int got = 0;
    for (int i = 0; i < 50; i++) {
        if (atomic_load(&ttl_expired_signal)) { got = 1; break; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert_true(got);

    /* Confirm the row is marked failed=1 in the DB. */
    db_result_t *r = NULL;
    assert_int_equal(db_execute(fix.db,
        "SELECT failed FROM push WHERE title = 'stale title'",
        NULL, &r), CUTILS_OK);
    assert_non_null(r);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0], "1");
    db_result_free(r);

    /* An expiry is a failure an application can count (1.4.0). */
    push_stats_t st;
    assert_int_equal(push_get_stats(&st), CUTILS_OK);
    assert_int_equal(st.failed, 1);
    assert_int_equal(st.expired, 1);
    assert_int_equal(st.sent, 0);
    assert_int_equal(st.pending, 0);

    log_stream_unregister(h);
    push_shutdown();
    free_fixture(&fix);
}

/* --- Shutdown stays responsive even when the worker is parked in
 *     pthread_cond_timedwait for a far-future retry. The pre-1.0.2
 *     send_one used sleep(4) which could block shutdown for seconds;
 *     the new design uses cond_timedwait so push_shutdown's signal
 *     wakes the worker immediately regardless of deadline. --- */

static void test_shutdown_responsive_with_future_retry(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);

    /* Row deferred 1 day into the future. Worker drains nothing on
     * startup, then enters cond_timedwait with a 24h deadline. */
    long long now = (long long)time(NULL);
    char ts_str[32], retry_str[32];
    snprintf(ts_str,    sizeof(ts_str),    "%lld", now);
    snprintf(retry_str, sizeof(retry_str), "%lld", now + 86400);
    const char *params[] = {
        ts_str, "tok", "usr", "86400",
        "deferred", "deferred title",
        "0", "0", "0", "0", retry_str, NULL
    };
    assert_int_equal(db_execute_non_query(fix.db,
        "INSERT INTO push (timestamp, token, user, ttl, message, title, "
        "failed, html, priority, attempts, next_retry_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL), CUTILS_OK);

    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Brief pause so the worker has time to enter cond_timedwait. */
    struct timespec brief = { 0, 200 * 1000 * 1000 };
    nanosleep(&brief, NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    push_shutdown();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long elapsed_ms =
        (long long)(t1.tv_sec - t0.tv_sec) * 1000 +
        (long long)(t1.tv_nsec - t0.tv_nsec) / 1000000;

    /* 2s ceiling is loose enough for slow CI but tight enough to
     * catch a sleep(N) regression. Real shutdown is sub-millisecond
     * here — the cond_signal wakes the worker, it sees push_stop=1
     * and returns. */
    assert_true(elapsed_ms < 2000);

    free_fixture(&fix);
}

/* The previous design wrote the URL-encoded POST body into a 4KB
 * stack buffer in send_one. With url-encoding expanding each UTF-8
 * byte up to 3x, a max-length title plus a max-length message of
 * mostly multi-byte characters could approach that bound. The
 * dynamic-sized buffer eliminates the failure mode entirely.
 *
 * This test feeds inputs that, after URL-encoding, are guaranteed to
 * exceed the old 4KB cap, then verifies the dynamic builder produced
 * a complete, NUL-terminated body containing every field intact and
 * no truncation at the 4096-byte mark. */
static void test_build_postfields_no_truncation_oversize(void **state)
{
    (void)state;

    /* curl_global_init / cleanup are reference-counted; safe to call
     * inside a unit test even if other tests have already initialized
     * libcurl through push_init. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    assert_non_null(curl);

    /* 2048 bytes of '!' (each byte URL-encodes to '%21'). After
     * encoding, the message alone is 6144 bytes — well past the old
     * 4KB fixed-buffer ceiling. The size is deliberately larger than
     * MAX_MSG_CHARS (1024) so the test acts as defense-in-depth: if
     * split_and_store is ever bypassed or its cap raised, the dynamic
     * builder still produces a complete body without truncation.
     * Title is similarly heavy-encoded for additional headroom. */
    char msg[2049];
    memset(msg, '!', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    char title[251];
    memset(title, '!', sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';

    char *body = cutils_push_build_postfields(
        curl,
        "ATOKEN1234567890ABCDEF1234567890",
        "AUSER1234567890ABCDEF1234567890",
        title, msg,
        "1700000000", "86400",
        1, 1);
    assert_non_null(body);

    size_t blen = strlen(body);
    assert_true(blen > 4096); /* Old fixed buffer would have truncated. */

    /* Spot-check structure: every parameter name appears, the encoded
     * message length matches expectation (3 bytes per source byte),
     * and the optional html / priority tails are present. */
    assert_non_null(strstr(body, "token=ATOKEN"));
    assert_non_null(strstr(body, "&user=AUSER"));
    assert_non_null(strstr(body, "&title=%21"));   /* '!' = %21 */
    assert_non_null(strstr(body, "&message=%21"));
    assert_non_null(strstr(body, "&timestamp=1700000000"));
    assert_non_null(strstr(body, "&ttl=86400"));
    assert_non_null(strstr(body, "&html=1"));
    assert_non_null(strstr(body, "&priority=1"));

    /* Verify no embedded NUL before the trailing one (would indicate
     * truncation that snprintf hid by NUL-terminating mid-content). */
    assert_int_equal(strlen(body), blen);

    free(body);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
}

/* --- Delivery counters (1.4.0) ---
 *
 * A message Pushover rejected was visible only as a log line, so an
 * application had nothing to alert on. These drive the worker against a
 * local HTTP listener answering with a chosen status and check that each
 * outcome lands in the right counter. */

typedef struct {
    int         listen_fd;
    int         port;
    _Atomic int status;      /* HTTP status the next request gets */
    _Atomic int requests;
    pthread_t   thread;
} fake_pushover_t;

static void *fake_pushover_main(void *arg)
{
    fake_pushover_t *fp = arg;
    for (;;) {
        int c = accept(fp->listen_fd, NULL, NULL);
        if (c < 0) return NULL;   /* listener shut down */

        /* Read headers, then Content-Length bytes of body, so curl has
         * finished sending before it sees the reply. */
        char buf[8192];
        size_t got = 0;
        const char *hdr_end = NULL;
        while (got < sizeof(buf) - 1) {
            ssize_t n = recv(c, buf + got, sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
            buf[got] = '\0';
            hdr_end = strstr(buf, "\r\n\r\n");
            if (!hdr_end) continue;
            const char *cl = strstr(buf, "Content-Length: ");
            size_t want = cl ? (size_t)strtoul(cl + 16, NULL, 10) : 0;
            if (got - (size_t)(hdr_end + 4 - buf) >= want) break;
        }

        int status = atomic_load(&fp->status);
        const char *body = status == 200 ? "{\"status\":1}"
                                         : "{\"status\":0,\"errors\":[\"token is invalid\"]}";
        char resp[256];
        int len = snprintf(resp, sizeof(resp),
                           "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                           "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           status, strlen(body), body);
        ssize_t w = send(c, resp, (size_t)len, 0);
        (void)w;
        atomic_fetch_add(&fp->requests, 1);
        close(c);
    }
}

static void fake_pushover_start(fake_pushover_t *fp, int status)
{
    fp->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(fp->listen_fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert_int_equal(bind(fp->listen_fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    assert_int_equal(listen(fp->listen_fd, 8), 0);
    socklen_t slen = sizeof(sa);
    assert_int_equal(getsockname(fp->listen_fd, (struct sockaddr *)&sa, &slen), 0);
    fp->port = ntohs(sa.sin_port);
    atomic_store(&fp->status, status);
    atomic_store(&fp->requests, 0);
    assert_int_equal(pthread_create(&fp->thread, NULL, fake_pushover_main, fp), 0);
}

static void fake_pushover_stop(fake_pushover_t *fp)
{
    shutdown(fp->listen_fd, SHUT_RDWR);
    close(fp->listen_fd);
    pthread_join(fp->thread, NULL);
}

/* Poll push_get_stats until pred holds or 10s pass. */
static int wait_stats(push_stats_t *st, int (*pred)(const push_stats_t *))
{
    for (int i = 0; i < 200; i++) {
        if (push_get_stats(st) == CUTILS_OK && pred(st)) return 1;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int one_sent(const push_stats_t *st)    { return st->sent == 1 && st->pending == 0; }
static int one_failed(const push_stats_t *st)  { return st->failed == 1 && st->pending == 0; }
static int one_retried(const push_stats_t *st) { return st->retried == 1; }

static void test_push_stats_count_outcomes(void **state)
{
    (void)state;
    fake_pushover_t fp;
    fake_pushover_start(&fp, 200);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/1/messages.json", fp.port);
    cutils_push_set_url(url);

    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    push_stats_t st;
    assert_int_equal(push_get_stats(&st), CUTILS_OK);
    assert_int_equal(st.sent, 0);
    assert_int_equal(st.failed, 0);

    /* Accepted: counted as sent, gone from the queue. */
    assert_int_equal(push_send("ok title", "ok body"), CUTILS_OK);
    assert_true(wait_stats(&st, one_sent));
    assert_int_equal(st.failed, 0);

    /* Rejected (4xx): given up on at once, counted as failed. Before
     * 1.4.0 this was the log line and nothing else. */
    atomic_store(&fp.status, 400);
    assert_int_equal(push_send("bad title", "bad body"), CUTILS_OK);
    assert_true(wait_stats(&st, one_failed));
    assert_int_equal(st.sent, 1);
    assert_int_equal(st.expired, 0);

    /* Server error (5xx): retried later, still pending. */
    atomic_store(&fp.status, 503);
    assert_int_equal(push_send("later title", "later body"), CUTILS_OK);
    assert_true(wait_stats(&st, one_retried));
    assert_int_equal(st.pending, 1);
    assert_int_equal(st.failed, 1);
    assert_int_equal(st.sent, 1);

    push_shutdown();
    cutils_push_set_url(NULL);
    fake_pushover_stop(&fp);
    assert_true(atomic_load(&fp.requests) >= 3);

    /* Shut down: counters survive for a final read, nothing is pending. */
    assert_int_equal(push_get_stats(&st), CUTILS_OK);
    assert_int_equal(st.sent, 1);
    assert_int_equal(st.pending, 0);

    free_fixture(&fix);
}

/* --- Failure causes in the log (1.5.1) ---
 *
 * A retried send logged "push transient failure (attempt N, retry in Ns)"
 * and nothing about why; only the permanent line carried the HTTP status
 * and body. These capture the worker's first failure line and check it
 * names the cause. */

static pthread_mutex_t g_fail_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_fail_line[512];

static void fail_line_cb(const char *ts, const char *level, const char *func,
                         const char *msg, void *userdata)
{
    (void)ts; (void)level; (void)func; (void)userdata;
    if (!msg) return;
    if (strncmp(msg, "push transient failure", 22) != 0 &&
        strncmp(msg, "push permanently failed", 23) != 0)
        return;
    pthread_mutex_lock(&g_fail_mu);
    if (!g_fail_line[0])
        snprintf(g_fail_line, sizeof(g_fail_line), "%s", msg);
    pthread_mutex_unlock(&g_fail_mu);
}

/* Sends one message to url and returns the first failure line logged,
 * in out, within 10 seconds. */
static void first_failure_line(const char *url, char *out, size_t out_sz)
{
    pthread_mutex_lock(&g_fail_mu);
    g_fail_line[0] = '\0';
    pthread_mutex_unlock(&g_fail_mu);
    cutils_push_set_url(url);

    push_fixture_t fix = make_fixture(1);
    int h = log_stream_register(fail_line_cb, NULL);
    assert_true(h >= 0);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    assert_int_equal(push_send("cause title", "cause body"), CUTILS_OK);

    out[0] = '\0';
    for (int i = 0; i < 200 && !out[0]; i++) {
        pthread_mutex_lock(&g_fail_mu);
        snprintf(out, out_sz, "%s", g_fail_line);
        pthread_mutex_unlock(&g_fail_mu);
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    log_stream_unregister(h);
    push_shutdown();
    cutils_push_set_url(NULL);
    free_fixture(&fix);
}

static void test_push_transient_http_logs_status_and_body(void **state)
{
    (void)state;
    fake_pushover_t fp;
    fake_pushover_start(&fp, 503);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/1/messages.json", fp.port);

    char line[512];
    first_failure_line(url, line, sizeof(line));
    fake_pushover_stop(&fp);

    assert_non_null(strstr(line, "push transient failure (HTTP 503, "));
    assert_non_null(strstr(line, "token is invalid"));
    assert_non_null(strstr(line, "attempt 1"));
    assert_non_null(strstr(line, "cause title"));
}

/* No HTTP status at all: the cause is curl's. A port that was bound and
 * closed again refuses the connection. */
static void test_push_transient_curl_logs_error(void **state)
{
    (void)state;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert_int_equal(bind(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    socklen_t slen = sizeof(sa);
    assert_int_equal(getsockname(fd, (struct sockaddr *)&sa, &slen), 0);
    int port = ntohs(sa.sin_port);
    close(fd);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/1/messages.json", port);
    char line[512];
    first_failure_line(url, line, sizeof(line));

    assert_non_null(strstr(line, "push transient failure (curl: "));
    assert_non_null(strstr(line, "attempt 1"));
}

/* The permanent line reads as it did before the cause moved into
 * send_one. */
static void test_push_permanent_logs_status_and_body(void **state)
{
    (void)state;
    fake_pushover_t fp;
    fake_pushover_start(&fp, 400);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/1/messages.json", fp.port);

    char line[512];
    first_failure_line(url, line, sizeof(line));
    fake_pushover_stop(&fp);

    assert_non_null(strstr(line, "push permanently failed (HTTP 400, "));
    assert_non_null(strstr(line, "token is invalid\"]}): cause title"));
}

static void test_push_stats_null_and_disabled(void **state)
{
    (void)state;
    assert_int_equal(push_get_stats(NULL), CUTILS_ERR_INVALID);

    /* No credentials: push is disabled and every counter reads zero. */
    push_fixture_t fix = make_fixture(0);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    push_stats_t st;
    memset(&st, 0xff, sizeof(st));
    assert_int_equal(push_get_stats(&st), CUTILS_OK);
    assert_int_equal(st.sent, 0);
    assert_int_equal(st.failed, 0);
    assert_int_equal(st.expired, 0);
    assert_int_equal(st.retried, 0);
    assert_int_equal(st.pending, 0);
    push_shutdown();
    free_fixture(&fix);
}

/* A count query that fails reports pending as -1 and returns the error,
 * with the counters still filled. */
static void test_push_stats_query_failure(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    push_shutdown();
    /* push_shutdown cleared the handle; re-init with the table gone. */
    assert_int_equal(db_execute_non_query(fix.db, "DROP TABLE push", NULL, NULL),
                     CUTILS_OK);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    push_stats_t st;
    assert_int_not_equal(push_get_stats(&st), CUTILS_OK);
    assert_int_equal(st.pending, -1);
    push_shutdown();
    free_fixture(&fix);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_push_send_no_op_when_uninitialized, teardown),
        cmocka_unit_test_teardown(test_push_shutdown_without_init, teardown),
        cmocka_unit_test_teardown(test_push_init_no_creds, teardown),
        cmocka_unit_test_teardown(test_push_send_no_op_when_creds_missing, teardown),
        cmocka_unit_test_teardown(test_push_send_enqueue, teardown),
        cmocka_unit_test_teardown(test_push_send_opts_override, teardown),
        cmocka_unit_test_teardown(test_push_send_long_message_split, teardown),
        cmocka_unit_test_teardown(test_push_send_opts_partial_creds, teardown),
        cmocka_unit_test_teardown(test_push_send_html_flag, teardown),
        cmocka_unit_test_teardown(test_push_send_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_html_and_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_defaults_zero, teardown),
        cmocka_unit_test_teardown(test_push_send_negative_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_after_shutdown_no_op, teardown),
        cmocka_unit_test_teardown(test_concurrent_push_send_multi_part_atomic, teardown),
        cmocka_unit_test_teardown(test_push_init_drains_preexisting_rows, teardown),
        cmocka_unit_test_teardown(test_ttl_expired_row_marked_failed, teardown),
        cmocka_unit_test_teardown(test_shutdown_responsive_with_future_retry, teardown),
        cmocka_unit_test_teardown(test_build_postfields_no_truncation_oversize, teardown),
        cmocka_unit_test_teardown(test_push_stats_count_outcomes, teardown),
        cmocka_unit_test_teardown(test_push_transient_http_logs_status_and_body, teardown),
        cmocka_unit_test_teardown(test_push_transient_curl_logs_error, teardown),
        cmocka_unit_test_teardown(test_push_permanent_logs_status_and_body, teardown),
        cmocka_unit_test_teardown(test_push_stats_null_and_disabled, teardown),
        cmocka_unit_test_teardown(test_push_stats_query_failure, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
