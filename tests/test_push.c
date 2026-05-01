#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cutils/push.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"

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

static void test_push_send_not_initialized(void **state)
{
    (void)state;
    /* push_send_opts with NULL db should fail */
    push_opts_t opts = {
        .title = "test",
        .message = "test msg",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_ERR_INVALID);
}

static void test_push_send_no_creds(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(0);

    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Even with explicit opts, if no creds it should fail */
    push_opts_t opts = {
        .title = "test",
        .message = "msg",
    };
    /* push is not running (no creds), so send_opts will hit the "not initialized" path
     * since push_db is NULL after init without creds */
    /* Actually push_db is still set, but push_running is 0 - send_opts checks push_db */
    /* The send should fail because credentials aren't available */
    int rc = push_send_opts(&opts);
    assert_int_not_equal(rc, CUTILS_OK);

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

/* --- Post-shutdown call must be rejected, not silently enqueued --- */

static void test_push_send_after_shutdown_rejected(void **state)
{
    (void)state;
    push_fixture_t fix = make_fixture(1);

    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);
    push_shutdown();

    /* Post-shutdown push_db is now NULL — sends must fail with
     * CUTILS_ERR_INVALID instead of inserting into the queue with
     * no live worker to drain. */
    push_opts_t opts = {
        .title = "post-shutdown",
        .message = "should not enqueue",
    };
    assert_int_equal(push_send_opts(&opts), CUTILS_ERR_INVALID);

    /* Verify no row was inserted. */
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
    /* No-creds fixture: push_init succeeds, but push_running stays 0
     * so the worker thread never starts. We get push_db set without
     * the worker draining rows behind us. */
    push_fixture_t fix = make_fixture(0);
    assert_int_equal(push_init(fix.db, fix.cfg), CUTILS_OK);

    /* Build two distinguishable >1024-char messages that split into 3+ chunks.
     * 2500 'A's / 'B's with newlines as split points. */
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

    /* Walk the queue in rowid order. The first message we see (A or B)
     * must have ALL its chunks before any chunk of the other — proving
     * the per-send tx serialized the inserts. */
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
            /* Came back to the first sender's chunks after switching to
             * the other — that's interleaving. Pre-fix this would
             * trigger; post-fix it must not. */
            db_result_free(r);
            push_shutdown();
            free_fixture(&fix);
            fail_msg("multi-part chunks interleaved across concurrent senders");
        }
    }

    db_result_free(r);
    push_shutdown();
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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_push_send_not_initialized, teardown),
        cmocka_unit_test_teardown(test_push_shutdown_without_init, teardown),
        cmocka_unit_test_teardown(test_push_init_no_creds, teardown),
        cmocka_unit_test_teardown(test_push_send_no_creds, teardown),
        cmocka_unit_test_teardown(test_push_send_enqueue, teardown),
        cmocka_unit_test_teardown(test_push_send_opts_override, teardown),
        cmocka_unit_test_teardown(test_push_send_long_message_split, teardown),
        cmocka_unit_test_teardown(test_push_send_opts_partial_creds, teardown),
        cmocka_unit_test_teardown(test_push_send_html_flag, teardown),
        cmocka_unit_test_teardown(test_push_send_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_html_and_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_defaults_zero, teardown),
        cmocka_unit_test_teardown(test_push_send_negative_priority, teardown),
        cmocka_unit_test_teardown(test_push_send_after_shutdown_rejected, teardown),
        cmocka_unit_test_teardown(test_concurrent_push_send_multi_part_atomic, teardown),
        cmocka_unit_test_teardown(test_push_init_drains_preexisting_rows, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
