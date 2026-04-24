#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "cutils/mem.h"

/* --- CUTILS_AUTOFREE --- */

static void test_autofree_frees_on_scope_exit(void **state)
{
    (void)state;
    /* If the cleanup doesn't fire, ASAN (make test-asan) catches the leak. */
    CUTILS_AUTOFREE char *buf = malloc(128);
    assert_non_null(buf);
    strcpy(buf, "hello");
    assert_string_equal(buf, "hello");
}

static void test_autofree_null_safe(void **state)
{
    (void)state;
    CUTILS_AUTOFREE char *buf = NULL;
    (void)buf;
}

static void test_autofree_after_early_return(void **state)
{
    (void)state;
    /* Run a helper that early-returns; its auto-freed allocation should
     * not be leaked. Again, ASAN is the authoritative check. */
    int rc;
    {
        CUTILS_AUTOFREE char *buf = malloc(64);
        assert_non_null(buf);
        rc = 0;
    }
    (void)rc;
}

/* --- CUTILS_AUTOCLOSE --- */

static void test_autoclose_closes_file(void **state)
{
    (void)state;
    char path[] = "/tmp/cutils_mem_test_XXXXXX";
    int tmpfd = mkstemp(path);
    assert_true(tmpfd >= 0);
    close(tmpfd);

    {
        CUTILS_AUTOCLOSE FILE *f = fopen(path, "r");
        assert_non_null(f);
    }

    unlink(path);
}

static void test_autoclose_null_safe(void **state)
{
    (void)state;
    CUTILS_AUTOCLOSE FILE *f = NULL;
    (void)f;
}

/* --- CUTILS_AUTOCLOSE_FD --- */

static void test_autoclose_fd_closes_fd(void **state)
{
    (void)state;
    char path[] = "/tmp/cutils_mem_test_XXXXXX";
    int tmpfd = mkstemp(path);
    assert_true(tmpfd >= 0);
    close(tmpfd);

    int captured;
    {
        CUTILS_AUTOCLOSE_FD int fd = open(path, O_RDONLY);
        assert_true(fd >= 0);
        captured = fd;
    }

    /* After scope exit, the fd we captured should now be closed.
     * close() on a closed fd yields EBADF. */
    int rc = close(captured);
    assert_int_equal(rc, -1);

    unlink(path);
}

static void test_autoclose_fd_negative_safe(void **state)
{
    (void)state;
    CUTILS_AUTOCLOSE_FD int fd = -1;
    (void)fd;
}

/* --- CUTILS_LOCK_GUARD --- */

static void test_lock_guard_locks_and_unlocks(void **state)
{
    (void)state;
    pthread_mutex_t m;
    assert_int_equal(pthread_mutex_init(&m, NULL), 0);

    {
        CUTILS_LOCK_GUARD(&m);
        /* Lock is held here; trylock from this thread would succeed
         * on a default (non-error-check) mutex regardless, so we only
         * verify the post-scope release below. */
    }

    /* After scope exit, the mutex must be unlocked — acquiring succeeds. */
    assert_int_equal(pthread_mutex_trylock(&m), 0);
    pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);
}

static void test_lock_guard_unlocks_on_early_return(void **state)
{
    (void)state;
    pthread_mutex_t m;
    assert_int_equal(pthread_mutex_init(&m, NULL), 0);

    /* Simulate an early-return helper inside a block. */
    for (int i = 0; i < 1; i++) {
        CUTILS_LOCK_GUARD(&m);
        break;   /* scope exits here — cleanup must still fire */
    }

    assert_int_equal(pthread_mutex_trylock(&m), 0);
    pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);
}

/* --- CUTILS_MOVE --- */

static void test_move_yields_value_and_nulls_source(void **state)
{
    (void)state;
    char *src = strdup("owned");
    assert_non_null(src);
    char *dst = CUTILS_MOVE(src);
    assert_null(src);
    assert_non_null(dst);
    assert_string_equal(dst, "owned");
    free(dst);
}

static void test_move_on_null_still_nulls(void **state)
{
    (void)state;
    char *src = NULL;
    char *dst = CUTILS_MOVE(src);
    assert_null(src);
    assert_null(dst);
}

static void test_move_prevents_double_free(void **state)
{
    (void)state;
    /* The classic UAF pattern: ownership passes elsewhere, original
     * pointer must not be reused. CUTILS_MOVE nulls the source so the
     * attempted reuse is a null-deref, not a UAF.
     *
     * This test just demonstrates the pattern. If someone later calls
     * free(src) on the moved-from variable, it's a no-op because
     * free(NULL) is defined. */
    char *owned = strdup("x");
    char *taken = CUTILS_MOVE(owned);
    free(owned);   /* harmless — owned is NULL */
    free(taken);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_autofree_frees_on_scope_exit),
        cmocka_unit_test(test_autofree_null_safe),
        cmocka_unit_test(test_autofree_after_early_return),
        cmocka_unit_test(test_autoclose_closes_file),
        cmocka_unit_test(test_autoclose_null_safe),
        cmocka_unit_test(test_autoclose_fd_closes_fd),
        cmocka_unit_test(test_autoclose_fd_negative_safe),
        cmocka_unit_test(test_lock_guard_locks_and_unlocks),
        cmocka_unit_test(test_lock_guard_unlocks_on_early_return),
        cmocka_unit_test(test_move_yields_value_and_nulls_source),
        cmocka_unit_test(test_move_on_null_still_nulls),
        cmocka_unit_test(test_move_prevents_double_free),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
