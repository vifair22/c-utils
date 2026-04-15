#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>
#include <string.h>

#include "cutils/error.h"

static void test_error_initial_state(void **state)
{
    (void)state;
    cutils_clear_error();
    assert_string_equal(cutils_get_error(), "");
}

static void test_set_error_returns_code(void **state)
{
    (void)state;
    int rc = set_error(CUTILS_ERR_IO, "test %s", "message");
    assert_int_equal(rc, CUTILS_ERR_IO);
}

static void test_set_error_stores_message(void **state)
{
    (void)state;
    set_error(CUTILS_ERR_DB, "db failed: %d", 42);
    assert_string_equal(cutils_get_error(), "db failed: 42");
}

static void test_clear_error(void **state)
{
    (void)state;
    set_error(CUTILS_ERR, "something");
    cutils_clear_error();
    assert_string_equal(cutils_get_error(), "");
}

static void test_error_overwrite(void **state)
{
    (void)state;
    set_error(CUTILS_ERR, "first");
    set_error(CUTILS_ERR_IO, "second");
    assert_string_equal(cutils_get_error(), "second");
}

static void test_err_name(void **state)
{
    (void)state;
    assert_string_equal(cutils_err_name(CUTILS_OK), "OK");
    assert_string_equal(cutils_err_name(CUTILS_ERR), "ERR");
    assert_string_equal(cutils_err_name(CUTILS_ERR_IO), "ERR_IO");
    assert_string_equal(cutils_err_name(CUTILS_ERR_DB), "ERR_DB");
    assert_string_equal(cutils_err_name(CUTILS_ERR_CONFIG), "ERR_CONFIG");
    assert_string_equal(cutils_err_name(CUTILS_ERR_NOT_FOUND), "ERR_NOT_FOUND");
}

static void test_set_error_errno_appends(void **state)
{
    (void)state;
    errno = ENOENT;
    set_error_errno(CUTILS_ERR_IO, "open(%s)", "/tmp/nope");
    const char *msg = cutils_get_error();
    /* Should contain both the formatted message and the errno string */
    assert_non_null(strstr(msg, "open(/tmp/nope)"));
    assert_non_null(strstr(msg, "No such file"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_error_initial_state),
        cmocka_unit_test(test_set_error_returns_code),
        cmocka_unit_test(test_set_error_stores_message),
        cmocka_unit_test(test_clear_error),
        cmocka_unit_test(test_error_overwrite),
        cmocka_unit_test(test_err_name),
        cmocka_unit_test(test_set_error_errno_appends),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
