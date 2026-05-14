#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cutils/db.h"
#include "cutils/error.h"

#define TEST_DB "/tmp/cutils_test.db"

static int setup(void **state)
{
    (void)state;
    unlink(TEST_DB);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    return 0;
}

static void test_open_close(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    int rc = db_open(&db, TEST_DB);
    assert_int_equal(rc, CUTILS_OK);
    assert_non_null(db);
    db_close(db);
}

static void test_create_table_and_insert(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(db_exec_raw(db,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"), CUTILS_OK);

    const char *params[] = { "hello", NULL };
    int affected = 0;
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)",
                             params, &affected),
        CUTILS_OK);
    assert_int_equal(affected, 1);

    db_close(db);
}

static void test_select(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"),
        CUTILS_OK);
    const char *p1[] = { "alice", NULL };
    const char *p2[] = { "bob", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p1, NULL),
        CUTILS_OK);
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (name) VALUES (?)", p2, NULL),
        CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM t ORDER BY name", NULL, &result),
        CUTILS_OK);

    assert_non_null(result);
    assert_int_equal(result->nrows, 2);
    assert_int_equal(result->ncols, 1);
    assert_string_equal(result->rows[0][0], "alice");
    assert_string_equal(result->rows[1][0], "bob");
    assert_string_equal(result->col_names[0], "name");

    db_result_free(result);
    db_close(db);
}

static void test_transaction_commit(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "inside_txn", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL),
        CUTILS_OK);
    assert_int_equal(db_commit(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "inside_txn");

    db_result_free(result);
    db_close(db);
}

static void test_transaction_rollback(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);

    assert_int_equal(db_begin(db), CUTILS_OK);
    const char *p[] = { "should_vanish", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p, NULL),
        CUTILS_OK);
    assert_int_equal(db_rollback(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
    db_close(db);
}

static void test_savepoint(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"),
        CUTILS_OK);
    assert_int_equal(db_begin(db), CUTILS_OK);

    const char *p1[] = { "kept", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p1, NULL),
        CUTILS_OK);

    assert_int_equal(db_savepoint(db, "sp1"), CUTILS_OK);
    const char *p2[] = { "rolled_back", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t (val) VALUES (?)", p2, NULL),
        CUTILS_OK);
    assert_int_equal(db_savepoint_rollback(db, "sp1"), CUTILS_OK);

    assert_int_equal(db_commit(db), CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT val FROM t", NULL, &result),
                     CUTILS_OK);
    assert_int_equal(result->nrows, 1);
    assert_string_equal(result->rows[0][0], "kept");

    db_result_free(result);
    db_close(db);
}

static void test_empty_result(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)"),
        CUTILS_OK);

    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT * FROM t", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_int_equal(result->nrows, 0);

    db_result_free(result);
    db_close(db);
}

static void test_auto_dbres_frees_on_scope_exit(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE t (v TEXT)"), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "INSERT INTO t VALUES ('auto')"),
                     CUTILS_OK);

    {
        CUTILS_AUTO_DBRES db_result_t *r = NULL;
        assert_int_equal(db_execute(db, "SELECT v FROM t", NULL, &r), CUTILS_OK);
        assert_non_null(r);
        assert_int_equal(r->nrows, 1);
        assert_string_equal(r->rows[0][0], "auto");
        /* No explicit db_result_free — cleanup fires on block exit.
         * ASAN validates under make test-asan. */
    }

    db_close(db);
}

static void test_auto_dbres_null_safe(void **state)
{
    (void)state;
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    (void)r;
    /* Cleanup must tolerate NULL without crashing. */
}

/* --- CUTILS_AUTO_DB_TX --- */

static int count_rows(cutils_db_t *db)
{
    db_result_t *r = NULL;
    assert_int_equal(db_execute(db, "SELECT COUNT(*) FROM tx", NULL, &r),
                     CUTILS_OK);
    int n = atoi(r->rows[0][0]);
    db_result_free(r);
    return n;
}

static void test_auto_db_tx_commit_persists(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('committed')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_rolls_back_on_early_return(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    for (int i = 0; i < 1; i++) {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('dropped')"), CUTILS_OK);
        break;  /* scope exits without commit — cleanup rolls back */
    }

    assert_int_equal(count_rows(db), 0);
    db_close(db);
}

static void test_auto_db_tx_commit_is_idempotent(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('once')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
        /* Second commit is a no-op — must not error. */
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_immediate_commit_persists(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin_immediate(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('committed')"), CUTILS_OK);
        assert_int_equal(db_tx_commit(&tx), CUTILS_OK);
    }

    assert_int_equal(count_rows(db), 1);
    db_close(db);
}

static void test_auto_db_tx_immediate_rolls_back_on_early_return(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE tx (v TEXT)"), CUTILS_OK);

    for (int i = 0; i < 1; i++) {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin_immediate(db, &tx), CUTILS_OK);
        assert_int_equal(
            db_exec_raw(db, "INSERT INTO tx VALUES ('dropped')"), CUTILS_OK);
        break;
    }

    assert_int_equal(count_rows(db), 0);
    db_close(db);
}


/* --- Concurrency regression: tx scope must hold the mutex --- */

typedef struct {
    cutils_db_t *db;
} race_arg_t;

static void *race_tx_thread(void *p)
{
    race_arg_t *a = p;
    {
        CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
        assert_int_equal(cutils_db_tx_begin(a->db, &tx), CUTILS_OK);
        const char *params[] = { "a", NULL };
        assert_int_equal(
            db_execute_non_query(a->db, "INSERT INTO race (v) VALUES (?)",
                                 params, NULL), CUTILS_OK);
        /* Hold the tx open long enough for the writer thread to attempt
         * its INSERT. Without the lock-held-across-tx fix, that INSERT
         * lands inside this tx and is rolled back along with row 'a'. */
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        /* Falls out of scope without commit — auto-cleanup rolls back. */
    }
    return NULL;
}

static void *race_writer_thread(void *p)
{
    race_arg_t *a = p;
    const char *params[] = { "b", NULL };
    assert_int_equal(
        db_execute_non_query(a->db, "INSERT INTO race (v) VALUES (?)",
                             params, NULL), CUTILS_OK);
    return NULL;
}

static void test_tx_holds_mutex_against_concurrent_writer(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(db_exec_raw(db, "CREATE TABLE race (v TEXT)"), CUTILS_OK);

    race_arg_t arg = { .db = db };
    pthread_t tA, tB;

    pthread_create(&tA, NULL, race_tx_thread, &arg);

    /* Let tA enter its tx scope before tB starts. */
    struct timespec gap = { 0, 50 * 1000 * 1000 };
    nanosleep(&gap, NULL);

    pthread_create(&tB, NULL, race_writer_thread, &arg);

    pthread_join(tA, NULL);
    pthread_join(tB, NULL);

    /* Only tB's row should survive — tA rolled back. */
    db_result_t *r = NULL;
    assert_int_equal(
        db_execute(db, "SELECT v FROM race", NULL, &r), CUTILS_OK);
    assert_int_equal(r->nrows, 1);
    assert_string_equal(r->rows[0][0], "b");
    db_result_free(r);

    db_close(db);
}

/* --- Streaming iterator (db_iter_*) --- */

static void seed_three_rows(cutils_db_t *db)
{
    assert_int_equal(
        db_exec_raw(db,
            "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, value TEXT)"),
        CUTILS_OK);
    const char *p1[] = { "alice", "a", NULL };
    const char *p2[] = { "bob",   "b", NULL };
    const char *p3[] = { "carol", "c", NULL };
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t(name, value) VALUES (?, ?)", p1, NULL),
        CUTILS_OK);
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t(name, value) VALUES (?, ?)", p2, NULL),
        CUTILS_OK);
    assert_int_equal(
        db_execute_non_query(db, "INSERT INTO t(name, value) VALUES (?, ?)", p3, NULL),
        CUTILS_OK);
}

static void test_iter_happy_path(void **state)
{
    /* Iterator usage is wrapped in its own block so CUTILS_AUTO_DB_ITER
     * fires BEFORE db_close — the iterator holds the connection mutex
     * and an open prepared statement; closing the DB while either is
     * still alive is UB. This pattern is documented in db.h. */
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT name, value FROM t ORDER BY id",
                          NULL, &it),
            CUTILS_OK);

        assert_int_equal(db_iter_ncols(it), 2);
        assert_string_equal(db_iter_col_name(it, 0), "name");
        assert_string_equal(db_iter_col_name(it, 1), "value");

        const char *expected_names[]  = { "alice", "bob", "carol" };
        const char *expected_values[] = { "a",     "b",   "c"     };
        const char **row = NULL;
        int seen = 0;
        while (db_iter_next(it, &row)) {
            assert_string_equal(row[0], expected_names[seen]);
            assert_string_equal(row[1], expected_values[seen]);
            seen++;
        }
        assert_int_equal(seen, 3);
        assert_int_equal(db_iter_error(it), 0);
    }

    db_close(db);
}

static void test_iter_empty_result(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"),
        CUTILS_OK);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT name FROM t WHERE id = 999",
                          NULL, &it),
            CUTILS_OK);

        const char **row = NULL;
        assert_int_equal(db_iter_next(it, &row), 0);
        assert_int_equal(db_iter_error(it), 0);
    }

    db_close(db);
}

static void test_iter_early_break_via_auto_cleanup(void **state)
{
    /* Caller breaks out of the loop after one row; CUTILS_AUTO_DB_ITER
     * is the only thing that cleans up. If finalize / mutex-unlock
     * didn't run, the next db_open + db call below would deadlock or
     * leak. The fact that this test returns means the cleanup ran. */
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT name FROM t ORDER BY id", NULL, &it),
            CUTILS_OK);

        const char **row = NULL;
        assert_int_equal(db_iter_next(it, &row), 1);
        assert_string_equal(row[0], "alice");
        /* Break — scope exit fires db_iter_end_p. */
    }

    /* If the iterator hadn't released the mutex, this would deadlock
     * (recursive mutex notwithstanding — we'd hang on the count
     * never reaching zero). */
    CUTILS_AUTO_DBRES db_result_t *r = NULL;
    assert_int_equal(
        db_execute(db, "SELECT name FROM t WHERE id = 1", NULL, &r),
        CUTILS_OK);
    assert_int_equal(r->nrows, 1);

    db_close(db);
}

static void test_iter_bad_sql(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);

    cutils_db_iter_t *it = NULL;
    int rc = db_iter_begin(db, "SELEKT * FROM nope", NULL, &it);
    assert_int_not_equal(rc, CUTILS_OK);
    assert_null(it);
    /* Mutex must have been released even though begin failed —
     * confirm by doing another op. */
    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t(id INTEGER)"),
        CUTILS_OK);

    db_close(db);
}

static void test_iter_null_column_returns_empty_string(void **state)
{
    /* db_execute returns "" for NULL columns (see db.c:157). The
     * iterator must match that contract so callers can swap APIs
     * without changing read code. */
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    assert_int_equal(
        db_exec_raw(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)"),
        CUTILS_OK);
    assert_int_equal(
        db_exec_raw(db, "INSERT INTO t(name) VALUES (NULL)"),
        CUTILS_OK);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT name FROM t", NULL, &it),
            CUTILS_OK);

        const char **row = NULL;
        assert_int_equal(db_iter_next(it, &row), 1);
        assert_non_null(row[0]);
        assert_string_equal(row[0], "");
    }

    db_close(db);
}

static void test_iter_with_bound_params(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        const char *params[] = { "bob", NULL };
        assert_int_equal(
            db_iter_begin(db, "SELECT name FROM t WHERE name = ?", params, &it),
            CUTILS_OK);

        const char **row = NULL;
        assert_int_equal(db_iter_next(it, &row), 1);
        assert_string_equal(row[0], "bob");
        assert_int_equal(db_iter_next(it, &row), 0);
    }

    db_close(db);
}

static void test_iter_end_safe_on_null(void **state)
{
    (void)state;
    db_iter_end(NULL); /* must not crash */
    cutils_db_iter_t *p = NULL;
    db_iter_end_p(&p); /* must not crash; p stays NULL */
    assert_null(p);
}

static void test_iter_ncols_and_col_name_bounds(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT id, name, value FROM t", NULL, &it),
            CUTILS_OK);

        assert_int_equal(db_iter_ncols(it), 3);
        assert_non_null(db_iter_col_name(it, 0));
        assert_non_null(db_iter_col_name(it, 2));
        /* Out-of-range returns NULL, doesn't crash. */
        assert_null(db_iter_col_name(it, -1));
        assert_null(db_iter_col_name(it, 3));
        assert_null(db_iter_col_name(it, 999));
        /* Same bounds discipline on NULL iter. */
        assert_int_equal(db_iter_ncols(NULL), 0);
        assert_null(db_iter_col_name(NULL, 0));
        assert_int_equal(db_iter_error(NULL), 0);
    }

    db_close(db);
}

/* Test the explicit-end path (rather than CUTILS_AUTO_DB_ITER) to
 * cover db_iter_end called directly. Also exercises iteration that
 * continues after seeing an error from a deliberately-failed step
 * (closing the db underneath the iterator would normally be UB; we
 * use a more controlled approach — bind a param to a column the
 * iterator step will then choke on... not really possible at this
 * layer. Instead this test just confirms the explicit-end path is
 * reachable). */
static void test_iter_explicit_end(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    cutils_db_iter_t *it = NULL;
    assert_int_equal(
        db_iter_begin(db, "SELECT name FROM t", NULL, &it),
        CUTILS_OK);

    /* Read one row then explicitly end before the natural stop. */
    const char **row = NULL;
    assert_int_equal(db_iter_next(it, &row), 1);
    db_iter_end(it);
    /* db_iter_end with already-finalized state should be no-op-safe
     * — pattern not commonly used but documented as safe. */

    db_close(db);
}

/* db_open_with_mode chmods the file unconditionally, so after the
 * call the file has exactly the requested mode regardless of whether
 * it was just created (subject to umask) or already existed. The two
 * tests below cover both shapes. */

static mode_t file_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mode & 0777;
}

static void test_db_open_with_mode_new_file(void **state)
{
    (void)state;
    /* Make sure no leftover file exists from a prior run. */
    unlink(TEST_DB);

    cutils_db_t *db = NULL;
    assert_int_equal(db_open_with_mode(&db, TEST_DB, 0600), CUTILS_OK);
    assert_non_null(db);
    /* The DB file should now exist with mode 0600 regardless of the
     * test runner's umask. */
    assert_int_equal((int)file_mode(TEST_DB), 0600);

    db_close(db);
}

static void test_db_open_with_mode_existing_file(void **state)
{
    /* Pre-create the file with permissive mode, then open with
     * restrictive mode. The post-open file mode should reflect the
     * caller's request, not the pre-existing perms. */
    (void)state;
    unlink(TEST_DB);

    cutils_db_t *db1 = NULL;
    assert_int_equal(db_open(&db1, TEST_DB), CUTILS_OK);
    db_close(db1);
    /* Force loose perms to simulate an old too-permissive DB file. */
    assert_int_equal(chmod(TEST_DB, 0644), 0);
    assert_int_equal((int)file_mode(TEST_DB), 0644);

    cutils_db_t *db2 = NULL;
    assert_int_equal(db_open_with_mode(&db2, TEST_DB, 0600), CUTILS_OK);
    assert_int_equal((int)file_mode(TEST_DB), 0600);
    db_close(db2);
}

/* Calling db_iter_next with row_out=NULL must not crash — useful for
 * "just walk to verify rows exist" style use. */
static void test_iter_next_null_row_out(void **state)
{
    (void)state;
    cutils_db_t *db = NULL;
    assert_int_equal(db_open(&db, TEST_DB), CUTILS_OK);
    seed_three_rows(db);

    {
        CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
        assert_int_equal(
            db_iter_begin(db, "SELECT name FROM t", NULL, &it),
            CUTILS_OK);

        int count = 0;
        while (db_iter_next(it, NULL)) count++;
        assert_int_equal(count, 3);
    }

    db_close(db);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_open_close, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_table_and_insert, setup, teardown),
        cmocka_unit_test_setup_teardown(test_select, setup, teardown),
        cmocka_unit_test_setup_teardown(test_transaction_commit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_transaction_rollback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_savepoint, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_result, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_dbres_frees_on_scope_exit, setup, teardown),
        cmocka_unit_test(test_auto_dbres_null_safe),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_commit_persists, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_rolls_back_on_early_return, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_commit_is_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_immediate_commit_persists, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auto_db_tx_immediate_rolls_back_on_early_return, setup, teardown),
        cmocka_unit_test_setup_teardown(test_tx_holds_mutex_against_concurrent_writer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_happy_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_empty_result, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_early_break_via_auto_cleanup, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_bad_sql, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_null_column_returns_empty_string, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_with_bound_params, setup, teardown),
        cmocka_unit_test(test_iter_end_safe_on_null),
        cmocka_unit_test_setup_teardown(test_iter_ncols_and_col_name_bounds, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_explicit_end, setup, teardown),
        cmocka_unit_test_setup_teardown(test_iter_next_null_row_out, setup, teardown),
        cmocka_unit_test_setup_teardown(test_db_open_with_mode_new_file, setup, teardown),
        cmocka_unit_test_setup_teardown(test_db_open_with_mode_existing_file, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
