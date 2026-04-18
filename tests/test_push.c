#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    log_init(NULL, LOG_INFO, 0);

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

    push_init(fix.db, fix.cfg);

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
    db_execute(fix.db, "SELECT token, user, ttl FROM push ORDER BY rowid DESC LIMIT 1",
               NULL, &result);
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
    db_execute(fix.db, "SELECT COUNT(*) FROM push WHERE title LIKE 'Split Test%'",
               NULL, &result);
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
    db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'HTML Test'",
        NULL, &result);
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
    db_execute(fix.db,
        "SELECT priority FROM push WHERE title = 'High Priority'",
        NULL, &result);
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
    db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'Combined'",
        NULL, &result);
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
    db_execute(fix.db,
        "SELECT html, priority FROM push WHERE title = 'Default'",
        NULL, &result);
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
    db_execute(fix.db,
        "SELECT priority FROM push WHERE title = 'Silent'",
        NULL, &result);
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
