#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"

/* Application write lock (1.8.0, CUTILS-2).
 *
 * The app under test runs its transactions the way c-backup does: its own
 * lock, then a raw BEGIN through db_exec_raw, which leaves the handle mutex
 * free between statements. Before 1.8.0 a c-utils writer on another thread
 * could run a statement in that window and land inside the app's
 * transaction. */

#define TEST_DB "/tmp/cutils_test_db_write_lock.db"

typedef struct {
    pthread_mutex_t mu;
    atomic_int      locks;
    atomic_int      unlocks;
    int             refuse;
} app_lock_t;

/* Held flag per thread, so the lock can answer DB_WRITE_LOCK_HELD. */
static _Thread_local int tls_held;

static int app_lock(void *ctx)
{
    app_lock_t *l = ctx;
    if (l->refuse) return -1;
    if (tls_held) return DB_WRITE_LOCK_HELD;
    pthread_mutex_lock(&l->mu);
    tls_held = 1;
    atomic_fetch_add(&l->locks, 1);
    return DB_WRITE_LOCK_ACQUIRED;
}

static void app_unlock(void *ctx)
{
    app_lock_t *l = ctx;
    tls_held = 0;
    atomic_fetch_add(&l->unlocks, 1);
    pthread_mutex_unlock(&l->mu);
}

static void app_lock_init(app_lock_t *l)
{
    memset(l, 0, sizeof(*l));
    pthread_mutex_init(&l->mu, NULL);
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&t, NULL);
}

static cutils_db_t *open_db(void)
{
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE app (v INTEGER); CREATE TABLE other (v INTEGER);"),
        CUTILS_OK);
    return db;
}

static int count_rows(cutils_db_t *db, const char *sql)
{
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, sql, NULL, &r), CUTILS_OK);
    assert_non_null(r);
    int n = atoi(r->rows[0][0]);
    db_result_free(r);
    return n;
}

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

/* --- Installation --- */

static void test_set_write_lock_validates(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);

    assert_int_equal(db_has_write_lock(db), 0);
    assert_int_equal(db_set_write_lock(db, app_lock, NULL, &l), CUTILS_ERR_INVALID);
    assert_int_equal(db_set_write_lock(db, NULL, app_unlock, &l), CUTILS_ERR_INVALID);
    assert_int_equal(db_set_write_lock(NULL, app_lock, app_unlock, &l), CUTILS_ERR_INVALID);
    assert_int_equal(db_has_write_lock(NULL), 0);
    assert_int_equal(db_has_write_lock(db), 0);

    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);
    assert_int_equal(db_has_write_lock(db), 1);

    /* Cleared: scopes take only the handle mutex again. */
    assert_int_equal(db_set_write_lock(db, NULL, NULL, NULL), CUTILS_OK);
    assert_int_equal(db_has_write_lock(db), 0);
    db_write_guard_t w = { 0 };
    assert_int_equal(db_write_begin(db, &w), CUTILS_OK);
    db_write_end(&w);
    assert_int_equal(atomic_load(&l.locks), 0);

    db_close(db);
}

static void test_write_begin_rejects_null(void **state)
{
    (void)state;
    db_write_guard_t w = { 0 };
    assert_int_equal(db_write_begin(NULL, &w), CUTILS_ERR_INVALID);
    assert_int_equal(w.active, 0);
    db_write_end(&w);   /* inactive guard: no-op */
    db_write_end(NULL); /* no-op */

    cutils_db_t *db = open_db();
    assert_int_equal(db_write_begin(db, NULL), CUTILS_ERR_INVALID);
    db_close(db);
}

/* --- Scope behaviour --- */

static void test_scope_takes_and_releases_lock(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);

    {
        CUTILS_AUTO_DB_WRITE db_write_guard_t w = { 0 };
        assert_int_equal(db_write_begin(db, &w), CUTILS_OK);
        assert_int_equal(atomic_load(&l.locks), 1);
        assert_int_equal(atomic_load(&l.unlocks), 0);
        db_write_end(&w);
        db_write_end(&w); /* idempotent */
        assert_int_equal(atomic_load(&l.unlocks), 1);
    }
    assert_int_equal(atomic_load(&l.unlocks), 1);

    db_close(db);
}

/* A thread that already holds the app lock nests: no second acquire, no
 * unlock at scope end, and the write joins the thread's own transaction. */
static void test_held_lock_nests_into_own_tx(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);

    assert_int_equal(app_lock(&l), DB_WRITE_LOCK_ACQUIRED);
    assert_int_equal(db_exec_raw(db, "BEGIN"), CUTILS_OK);
    {
        CUTILS_AUTO_DB_WRITE db_write_guard_t w = { 0 };
        assert_int_equal(db_write_begin(db, &w), CUTILS_OK);
        assert_int_equal(db_execute_non_query(db,
            "INSERT INTO other VALUES (1)", NULL, NULL), CUTILS_OK);
    }
    assert_int_equal(atomic_load(&l.locks), 1);
    assert_int_equal(atomic_load(&l.unlocks), 0);
    assert_int_equal(db_exec_raw(db, "ROLLBACK"), CUTILS_OK);
    app_unlock(&l);

    assert_int_equal(count_rows(db, "SELECT count(*) FROM other"), 0);
    db_close(db);
}

static void *query_thread(void *arg)
{
    cutils_db_t *db = arg;
    db_result_t *r = NULL;
    int rc = db_execute(db, "SELECT 1", NULL, &r);
    db_result_free(r);
    return (void *)(intptr_t)rc;
}

/* A refusing lock fails the scope without leaving the handle mutex held. */
static void test_refused_lock_fails_cleanly(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);
    l.refuse = 1;
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);

    db_write_guard_t w = { 0 };
    assert_int_equal(db_write_begin(db, &w), CUTILS_ERR_DB);
    assert_int_equal(w.active, 0);
    db_write_end(&w);

    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
    assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_ERR_DB);

    pthread_t t;
    assert_int_equal(pthread_create(&t, NULL, query_thread, db), 0);
    void *ret = NULL;
    pthread_join(t, &ret);
    assert_int_equal((int)(intptr_t)ret, CUTILS_OK);

    db_close(db);
}

/* The scoped transaction API takes the app lock for its whole span. */
static void test_tx_scope_uses_lock(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(atomic_load(&l.locks), 1);
        assert_int_equal(db_execute_non_query(db,
            "INSERT INTO app VALUES (1)", NULL, NULL), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
        assert_int_equal(atomic_load(&l.unlocks), 1);
    }
    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin_immediate(db, &tx), CUTILS_OK);
        assert_int_equal(db_execute_non_query(db,
            "INSERT INTO app VALUES (2)", NULL, NULL), CUTILS_OK);
        /* scope exit rolls back and releases */
    }
    assert_int_equal(atomic_load(&l.locks), 2);
    assert_int_equal(atomic_load(&l.unlocks), 2);
    assert_int_equal(count_rows(db, "SELECT count(*) FROM app"), 1);

    /* BEGIN failing (already in a transaction) releases the lock too. */
    assert_int_equal(app_lock(&l), DB_WRITE_LOCK_ACQUIRED);
    assert_int_equal(db_exec_raw(db, "BEGIN"), CUTILS_OK);
    app_unlock(&l);
    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_ERR_DB);
    }
    assert_int_equal(atomic_load(&l.locks), 4);
    assert_int_equal(atomic_load(&l.unlocks), 4);
    assert_int_equal(db_exec_raw(db, "ROLLBACK"), CUTILS_OK);

    db_close(db);
}

/* --- The CUTILS-2 scenario --- */

typedef struct {
    cutils_db_t *db;
    atomic_int   started;
    int          rc;
} writer_arg_t;

static void *scoped_writer(void *arg)
{
    writer_arg_t *a = arg;
    atomic_store(&a->started, 1);
    CUTILS_AUTO_DB_WRITE db_write_guard_t w = { 0 };
    a->rc = db_write_begin(a->db, &w);
    if (a->rc == CUTILS_OK)
        a->rc = db_execute_non_query(a->db, "INSERT INTO other VALUES (7)",
                                     NULL, NULL);
    return NULL;
}

/* A write scope on another thread waits out the app's transaction instead
 * of joining it, so the app's rollback does not take the write with it. */
static void test_scope_waits_out_app_tx(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    app_lock_t l;
    app_lock_init(&l);
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);

    assert_int_equal(app_lock(&l), DB_WRITE_LOCK_ACQUIRED);
    assert_int_equal(db_exec_raw(db, "BEGIN"), CUTILS_OK);
    assert_int_equal(db_execute_non_query(db, "INSERT INTO app VALUES (1)",
                                          NULL, NULL), CUTILS_OK);

    writer_arg_t a = { .db = db };
    pthread_t t;
    assert_int_equal(pthread_create(&t, NULL, scoped_writer, &a), 0);
    while (!atomic_load(&a.started)) nap_ms(1);
    nap_ms(100);

    assert_int_equal(db_exec_raw(db, "ROLLBACK"), CUTILS_OK);
    app_unlock(&l);
    pthread_join(t, NULL);

    assert_int_equal(a.rc, CUTILS_OK);
    assert_int_equal(count_rows(db, "SELECT count(*) FROM app"), 0);
    assert_int_equal(count_rows(db, "SELECT count(*) FROM other"), 1);
    db_close(db);
}

/* The log writer thread: a line logged while the app's transaction is open
 * survives that transaction's rollback. */
static void test_log_line_survives_app_rollback(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);
    app_lock_t l;
    app_lock_init(&l);
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);
    assert_int_equal(log_init(db, LOG_INFO, 0), CUTILS_OK);

    assert_int_equal(app_lock(&l), DB_WRITE_LOCK_ACQUIRED);
    assert_int_equal(db_exec_raw(db, "BEGIN"), CUTILS_OK);
    assert_int_equal(db_execute_non_query(db, "INSERT INTO app VALUES (1)",
                                          NULL, NULL), CUTILS_OK);
    log_write(LOG_INFO, "t", "logged during the app tx");
    nap_ms(150); /* the writer reaches the lock and waits */
    assert_int_equal(db_exec_raw(db, "ROLLBACK"), CUTILS_OK);
    app_unlock(&l);

    log_shutdown();
    assert_int_equal(count_rows(db, "SELECT count(*) FROM app"), 0);
    assert_int_equal(count_rows(db,
        "SELECT count(*) FROM logs WHERE message = 'logged during the app tx'"), 1);
    db_close(db);
}

/* With the lock refusing, the writer drops the batch rather than writing
 * outside the app's control. */
static void test_log_writer_drops_on_refusal(void **state)
{
    (void)state;
    cutils_db_t *db = open_db();
    assert_int_equal(db_run_lib_migrations(db), CUTILS_OK);
    app_lock_t l;
    app_lock_init(&l);
    l.refuse = 1;
    assert_int_equal(db_set_write_lock(db, app_lock, app_unlock, &l), CUTILS_OK);
    assert_int_equal(log_init(db, LOG_INFO, 0), CUTILS_OK);
    log_write(LOG_INFO, "t", "refused line");
    log_shutdown();
    assert_int_equal(count_rows(db,
        "SELECT count(*) FROM logs WHERE message = 'refused line'"), 0);
    db_close(db);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_set_write_lock_validates, teardown),
        cmocka_unit_test_teardown(test_write_begin_rejects_null, teardown),
        cmocka_unit_test_teardown(test_scope_takes_and_releases_lock, teardown),
        cmocka_unit_test_teardown(test_held_lock_nests_into_own_tx, teardown),
        cmocka_unit_test_teardown(test_refused_lock_fails_cleanly, teardown),
        cmocka_unit_test_teardown(test_tx_scope_uses_lock, teardown),
        cmocka_unit_test_teardown(test_scope_waits_out_app_tx, teardown),
        cmocka_unit_test_teardown(test_log_line_survives_app_rollback, teardown),
        cmocka_unit_test_teardown(test_log_writer_drops_on_refusal, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
