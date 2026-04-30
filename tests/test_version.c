#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <regex.h>
#include <string.h>

#include "cutils/version.h"

static void test_version_non_null_and_non_empty(void **state)
{
    (void)state;
    const char *v = cutils_version();
    assert_non_null(v);
    assert_true(strlen(v) > 0);
}

static void test_version_format(void **state)
{
    (void)state;
    /* Format: <major>.<minor>.<patch>_<YYYYMMDD>.<HHMM>.<type> */
    regex_t re;
    int rc = regcomp(&re,
        "^[0-9]+\\.[0-9]+\\.[0-9]+_[0-9]{8}\\.[0-9]{4}\\.(release|debug|asan|coverage)$",
        REG_EXTENDED);
    assert_int_equal(rc, 0);

    rc = regexec(&re, cutils_version(), 0, NULL, 0);
    regfree(&re);
    assert_int_equal(rc, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version_non_null_and_non_empty),
        cmocka_unit_test(test_version_format),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
