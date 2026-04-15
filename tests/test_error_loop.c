#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>

#include "cutils/error_loop.h"

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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_no_callback_below_threshold, setup),
        cmocka_unit_test_setup(test_callback_at_threshold, setup),
        cmocka_unit_test_setup(test_different_error_resets, setup),
        cmocka_unit_test_setup(test_success_resets, setup),
        cmocka_unit_test_setup(test_normalization_matches, setup),
        cmocka_unit_test_setup(test_normalization_uuid, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
