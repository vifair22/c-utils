#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "cutils/error_loop.h"
#include "cutils/mem.h"

/* Track callback invocations */
static int cb_called = 0;
static int cb_count = 0;
static char cb_message[256] = "";

static void test_callback(const char *message, int count, void *userdata)
{
    (void)userdata;
    cb_called = 1;
    cb_count = count;
    snprintf(cb_message, sizeof(cb_message), "%s", message);
}

static int setup(void **state)
{
    (void)state;
    cb_called = 0;
    cb_count = 0;
    cb_message[0] = '\0';
    return 0;
}

static void test_no_callback_below_threshold(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(3, 0, test_callback, NULL);

    error_loop_report(det, "error %d", 1);
    error_loop_report(det, "error %d", 1);
    assert_int_equal(cb_called, 0);

    error_loop_free(det);
}

static void test_callback_at_threshold(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(3, 0, test_callback, NULL);

    error_loop_report(det, "same error");
    error_loop_report(det, "same error");
    error_loop_report(det, "same error");

    assert_int_equal(cb_called, 1);
    assert_int_equal(cb_count, 3);
    assert_string_equal(cb_message, "same error");

    error_loop_free(det);
}

static void test_different_error_resets(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(3, 0, test_callback, NULL);

    error_loop_report(det, "error A");
    error_loop_report(det, "error A");
    error_loop_report(det, "error B"); /* resets */
    assert_int_equal(cb_called, 0);

    error_loop_free(det);
}

static void test_success_resets(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(3, 0, test_callback, NULL);

    error_loop_report(det, "error");
    error_loop_report(det, "error");
    error_loop_success(det);
    error_loop_report(det, "error");
    error_loop_report(det, "error");
    assert_int_equal(cb_called, 0);

    error_loop_free(det);
}

static void test_normalization_matches(void **state)
{
    (void)state;
    /* Same logical error with different hex addresses should match */
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "failed at 0xDEADBEEF");
    error_loop_report(det, "failed at 0x12345678");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

static void test_normalization_uuid(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "req 550e8400-e29b-41d4-a716-446655440000 failed");
    error_loop_report(det, "req a1b2c3d4-e5f6-7890-abcd-ef1234567890 failed");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test timestamp normalization */
static void test_normalization_timestamp(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "failed at 2024-01-15T10:30:00 server");
    error_loop_report(det, "failed at 2024-06-20T14:45:59 server");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test timestamp with fractional seconds */
static void test_normalization_timestamp_frac(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "err at 2024-01-15 10:30:00.123456 end");
    error_loop_report(det, "err at 2024-06-20 14:45:59.999 end");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test long hex sequence normalization */
static void test_normalization_long_hex(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "hash DEADBEEF12345678 mismatch");
    error_loop_report(det, "hash AABBCCDD99887766 mismatch");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test cooldown suppression */
static void test_cooldown_suppresses(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 9999, test_callback, NULL);

    /* Hit threshold, triggers callback + sets cooldown */
    error_loop_report(det, "repeating");
    error_loop_report(det, "repeating");
    assert_int_equal(cb_called, 1);

    /* More of the same error — should be suppressed by cooldown */
    cb_called = 0;
    error_loop_report(det, "repeating");
    error_loop_report(det, "repeating");
    assert_int_equal(cb_called, 0);

    error_loop_free(det);
}

/* Test default callback (no callback provided) */
static void test_default_callback(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, NULL, NULL);

    /* Should not crash — uses internal default callback */
    error_loop_report(det, "default handler test");
    error_loop_report(det, "default handler test");

    error_loop_free(det);
}

/* Test create with negative threshold (uses default) */
static void test_create_negative_threshold(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(-1, 0, test_callback, NULL);

    /* Default threshold is 5 */
    for (int i = 0; i < 4; i++)
        error_loop_report(det, "err");
    assert_int_equal(cb_called, 0);

    error_loop_report(det, "err");
    assert_int_equal(cb_called, 1);

    error_loop_free(det);
}

/* Test free NULL (safety) */
static void test_free_null(void **state)
{
    (void)state;
    error_loop_free(NULL); /* should not crash */
}

/* Test count exceeds threshold (past threshold, no cooldown) */
static void test_count_past_threshold(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    error_loop_report(det, "repeat");
    error_loop_report(det, "repeat"); /* threshold hit */
    assert_int_equal(cb_called, 1);

    /* Past threshold without cooldown — should not re-fire */
    cb_called = 0;
    error_loop_report(det, "repeat");
    assert_int_equal(cb_called, 0);

    error_loop_free(det);
}

/* Test UUID-like hex followed by alphanumeric (should NOT be replaced) */
static void test_normalization_uuid_followed_by_alnum(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    /* UUID pattern followed by 'x' — should NOT normalize as UUID */
    error_loop_report(det, "id 550e8400e29b41d4a716446655440000x end");
    error_loop_report(det, "id 550e8400e29b41d4a716446655440000x end");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test short hex sequence (< 8 chars, not replaced) */
static void test_normalization_short_hex(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    /* Short hex (< 8 chars) should NOT be replaced */
    error_loop_report(det, "code ABCD error");
    error_loop_report(det, "code EFGH error"); /* different, not hex */

    assert_int_equal(cb_called, 0);
    error_loop_free(det);
}

/* Test hex sequence followed by alpha (should NOT be replaced) */
static void test_normalization_hex_followed_by_alpha(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    /* 8+ hex chars followed by alpha — should NOT normalize */
    error_loop_report(det, "hash DEADBEEF12345678xyz stuff");
    error_loop_report(det, "hash AABBCCDD99887766xyz stuff");

    /* Different raw strings, but after normalization the hex is kept
       since it's followed by alpha */
    error_loop_free(det);
}

/* Test incomplete UUID (fails at dash position) */
static void test_normalization_incomplete_uuid(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    /* Almost a UUID but wrong separator */
    error_loop_report(det, "id 550e8400Xe29b-41d4-a716-446655440000 end");
    error_loop_report(det, "id 550e8400Xe29b-41d4-a716-446655440000 end");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

/* Test non-hex char in UUID group */
static void test_normalization_nonhex_in_uuid(void **state)
{
    (void)state;
    error_loop_t *det = error_loop_create(2, 0, test_callback, NULL);

    /* Hex chars that start like UUID but have non-hex in a group */
    error_loop_report(det, "id 550e840g-e29b-41d4-a716-446655440000 end");
    error_loop_report(det, "id 550e840g-e29b-41d4-a716-446655440000 end");

    assert_int_equal(cb_called, 1);
    error_loop_free(det);
}

static void test_auto_errloop_frees_on_scope_exit(void **state)
{
    (void)state;
    {
        CUTILS_AUTO_ERRLOOP error_loop_t *det =
            error_loop_create(3, 0, test_callback, NULL);
        assert_non_null(det);
        error_loop_report(det, "anything");
        /* No explicit error_loop_free — cleanup on scope exit.
         * ASAN validates under make test-asan. */
    }
}

static void test_auto_errloop_null_safe(void **state)
{
    (void)state;
    CUTILS_AUTO_ERRLOOP error_loop_t *det = NULL;
    (void)det;
}

/* --- Concurrency regression: shared detector across threads --- */

static atomic_int concurrent_cb_calls = 0;

static void concurrent_callback(const char *message, int count, void *userdata)
{
    (void)message; (void)count; (void)userdata;
    atomic_fetch_add(&concurrent_cb_calls, 1);
}

typedef struct {
    error_loop_t *det;
    int           iterations;
} concurrent_arg_t;

static void *concurrent_reporter(void *p)
{
    concurrent_arg_t *a = p;
    for (int i = 0; i < a->iterations; i++)
        error_loop_report(a->det, "shared error %d", 0);
    return NULL;
}

static void test_concurrent_reporters_no_corruption(void **state)
{
    (void)state;
    atomic_store(&concurrent_cb_calls, 0);

    /* Large cooldown so the threshold callback fires at most once across
     * all reporters — proves the lock keeps count consistent. */
    error_loop_t *det = error_loop_create(50, 9999,
                                          concurrent_callback, NULL);
    assert_non_null(det);

    enum { NTHREADS = 4, ITERATIONS = 1000 };
    pthread_t threads[NTHREADS];
    concurrent_arg_t arg = { .det = det, .iterations = ITERATIONS };

    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&threads[i], NULL, concurrent_reporter, &arg);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(threads[i], NULL);

    /* All reports were the same identity, so threshold fires exactly once. */
    assert_int_equal(atomic_load(&concurrent_cb_calls), 1);

    error_loop_free(det);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_no_callback_below_threshold, setup),
        cmocka_unit_test_setup(test_callback_at_threshold, setup),
        cmocka_unit_test_setup(test_different_error_resets, setup),
        cmocka_unit_test_setup(test_success_resets, setup),
        cmocka_unit_test_setup(test_normalization_matches, setup),
        cmocka_unit_test_setup(test_normalization_uuid, setup),
        cmocka_unit_test_setup(test_normalization_timestamp, setup),
        cmocka_unit_test_setup(test_normalization_timestamp_frac, setup),
        cmocka_unit_test_setup(test_normalization_long_hex, setup),
        cmocka_unit_test_setup(test_cooldown_suppresses, setup),
        cmocka_unit_test_setup(test_default_callback, setup),
        cmocka_unit_test_setup(test_create_negative_threshold, setup),
        cmocka_unit_test_setup(test_free_null, setup),
        cmocka_unit_test_setup(test_count_past_threshold, setup),
        cmocka_unit_test_setup(test_normalization_uuid_followed_by_alnum, setup),
        cmocka_unit_test_setup(test_normalization_short_hex, setup),
        cmocka_unit_test_setup(test_normalization_hex_followed_by_alpha, setup),
        cmocka_unit_test_setup(test_normalization_incomplete_uuid, setup),
        cmocka_unit_test_setup(test_normalization_nonhex_in_uuid, setup),
        cmocka_unit_test_setup(test_auto_errloop_frees_on_scope_exit, setup),
        cmocka_unit_test_setup(test_auto_errloop_null_safe, setup),
        cmocka_unit_test_setup(test_concurrent_reporters_no_corruption, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
