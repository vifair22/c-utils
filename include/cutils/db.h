#ifndef CUTILS_DB_H
#define CUTILS_DB_H

#include <stddef.h>
#include <stdint.h>

#include "cutils/mem.h"

/* --- Database subsystem ---
 *
 * Mutex-protected SQLite connection with WAL mode.
 * All access serialized through the mutex — safe to call from any thread.
 *
 * Query results are returned as a simple row/column structure that the
 * caller frees with db_result_free(). */

/* Opaque database handle */
typedef struct cutils_db cutils_db_t;

/* Query result set */
typedef struct {
    char ***rows;      /* rows[row_idx][col_idx], NULL-terminated strings */
    int     nrows;
    int     ncols;
    char  **col_names; /* col_names[col_idx] */
} db_result_t;

/* Open a database connection. Creates the file if it doesn't exist.
 * Enables WAL mode and sets a 5-second busy timeout.
 * Returns CUTILS_OK on success, error code on failure. */
CUTILS_MUST_USE
int db_open(cutils_db_t **db, const char *path);

/* Close and free the database handle. Safe to call with NULL. */
void db_close(cutils_db_t *db);

/* Execute a SELECT query. Caller must free the result with db_result_free().
 * params is a NULL-terminated array of string bind values, or NULL for no params.
 * Returns CUTILS_OK on success. On error, *result is set to NULL. */
CUTILS_MUST_USE
int db_execute(cutils_db_t *db, const char *sql, const char **params,
               db_result_t **result);

/* Execute an INSERT/UPDATE/DELETE query. Returns CUTILS_OK on success.
 * If affected is non-NULL, stores the number of rows affected.
 * params is a NULL-terminated array of string bind values, or NULL. */
CUTILS_MUST_USE
int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected);

/* Execute raw SQL (multiple statements, no params, no result).
 * Used internally by the migration runner. Returns CUTILS_OK on success. */
CUTILS_MUST_USE
int db_exec_raw(cutils_db_t *db, const char *sql);

/* Free a query result set. Safe to call with NULL. */
void db_result_free(db_result_t *result);

/* Cleanup helper for __attribute__((cleanup(...))).
 * Frees *result and sets *result to NULL. Use via CUTILS_AUTO_DBRES. */
void db_result_free_p(db_result_t **result);

/* Scoped cleanup for db_result_t *. Declare a result variable with this
 * attribute and db_result_free() runs automatically on scope exit:
 *
 *   CUTILS_AUTO_DBRES db_result_t *r = NULL;
 *   if (db_execute(db, sql, NULL, &r) != CUTILS_OK) return -1;
 *   / * use r — freed on any return path * /
 */
#define CUTILS_AUTO_DBRES __attribute__((cleanup(db_result_free_p)))

/* Begin/commit/rollback transactions */
CUTILS_MUST_USE int db_begin   (cutils_db_t *db);
CUTILS_MUST_USE int db_commit  (cutils_db_t *db);
CUTILS_MUST_USE int db_rollback(cutils_db_t *db);

/* --- Scoped transaction guard ---
 *
 * Pairs BEGIN with an automatic ROLLBACK on scope exit unless
 * db_tx_commit() has been called. Eliminates the common bug where
 * an early return leaves a transaction dangling.
 *
 *   CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
 *   if (cutils_db_tx_begin(db, &tx) != CUTILS_OK) return -1;
 *
 *   if (db_execute_non_query(db, sql1, p1, NULL) != CUTILS_OK) return -1;
 *   if (db_execute_non_query(db, sql2, p2, NULL) != CUTILS_OK) return -1;
 *
 *   return db_tx_commit(&tx);
 *
 * Any early return triggers the cleanup, which rolls back. Explicit
 * commit marks the transaction finalized so the cleanup is a no-op.
 *
 * Deferred vs. immediate: cutils_db_tx_begin issues plain BEGIN, which
 * only takes a SHARED lock on the first read and upgrades on the first
 * write. For read-modify-write sequences where two handlers could race
 * (SELECT, SELECT, UPDATE, UPDATE), use cutils_db_tx_begin_immediate —
 * it acquires RESERVED at BEGIN time, so the second caller blocks at
 * BEGIN (respecting busy_timeout) and its SELECT observes the first
 * caller's committed state. */
typedef struct {
    cutils_db_t *db;
    int          active;     /* 1 if BEGIN succeeded */
    int          finalized;  /* 1 once commit or rollback has run */
} cutils_db_tx_t;

/* Begin a scoped deferred transaction. Initializes *tx and issues BEGIN.
 * Returns CUTILS_OK on success; on failure, tx is left inactive and
 * the cleanup will be a no-op. */
CUTILS_MUST_USE
int cutils_db_tx_begin(cutils_db_t *db, cutils_db_tx_t *tx);

/* Begin a scoped immediate transaction. Issues BEGIN IMMEDIATE, which
 * takes a RESERVED lock up front. Use when the transaction performs a
 * read-before-write (row merge, check-then-update) and must serialize
 * against concurrent writers — deferred BEGIN cannot guarantee that the
 * SELECT observes writes committed by a racing writer. */
CUTILS_MUST_USE
int cutils_db_tx_begin_immediate(cutils_db_t *db, cutils_db_tx_t *tx);

/* Commit a scoped transaction. On success, marks tx finalized so the
 * cleanup does not roll back. Idempotent. Returns CUTILS_OK or the
 * underlying db_commit error. */
CUTILS_MUST_USE
int db_tx_commit(cutils_db_tx_t *tx);

/* Cleanup helper invoked by CUTILS_AUTO_DB_TX.
 * Rolls back the transaction if it was begun and never finalized. */
void cutils_db_tx_end_p(cutils_db_tx_t *tx);

#define CUTILS_AUTO_DB_TX __attribute__((cleanup(cutils_db_tx_end_p)))

/* --- Streaming query iterator ---
 *
 * Same param-binding contract as db_execute, but delivers rows
 * one-at-a-time without materializing the full result set. Use this
 * for SELECTs with unknown or large row counts; use db_execute when
 * the result is known small and you want an indexable array you can
 * pass around.
 *
 *   CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
 *   const char *params[] = { "active", NULL };
 *   if (db_iter_begin(db, "SELECT id, name FROM users WHERE status = ?",
 *                     params, &it) != CUTILS_OK)
 *       return -1;
 *
 *   const char **row;
 *   while (db_iter_next(it, &row)) {
 *       / * row[0] = "id" column, row[1] = "name" column.
 *        * Strings are valid until the next db_iter_next call. * /
 *       handle(row[0], row[1]);
 *   }
 *   if (db_iter_error(it))
 *       return -1;  / * sqlite error, message in cutils_get_error() * /
 *
 * Memory: O(1) heap regardless of row count. db_execute is O(n) —
 * every row's cells are strdup'd. The iterator hands out pointers
 * into sqlite's internal column buffers, which sqlite recycles on
 * each step.
 *
 * Locking: the iterator holds the connection mutex for its entire
 * lifetime. No other db_* call on the same connection can proceed
 * while an iterator is open. Keep iterator scopes short — never
 * span network I/O, condition waits, or other potentially-blocking
 * work inside the loop body.
 *
 * Lifetime-ordering rule: the iterator must end BEFORE db_close on
 * the same connection. Either let CUTILS_AUTO_DB_ITER fire in a
 * nested block:
 *
 *     {
 *         CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
 *         db_iter_begin(db, ...);
 *         // use
 *     }  // iter end fires here
 *     db_close(db);
 *
 * Or call db_iter_end(it) explicitly before db_close. Closing the
 * db while an iterator is open is UB — the iterator still holds
 * the mutex and a prepared statement against the now-freed
 * connection. */

typedef struct cutils_db_iter cutils_db_iter_t;

/* Begin iteration. params is a NULL-terminated array of bind values
 * (same shape as db_execute), or NULL for no params. On success
 * *iter receives a heap-allocated iterator that must be released
 * with db_iter_end (or via CUTILS_AUTO_DB_ITER). On error *iter is
 * NULL and the message is in cutils_get_error(). */
CUTILS_MUST_USE
int  db_iter_begin(cutils_db_t *db, const char *sql,
                   const char **params, cutils_db_iter_t **iter);

/* Step to the next row. Returns 1 (true) if *row_out now points at
 * a fresh row, 0 at natural end-of-rows OR on sqlite step error
 * (callers who care can disambiguate via db_iter_error). NULL
 * columns are returned as the empty string "" to match
 * db_execute's contract. The row_out array is the same pointer for
 * every call within one iterator; only the contents change. */
int  db_iter_next (cutils_db_iter_t *iter, const char ***row_out);

/* Number of columns in the result set. Constant across iteration. */
int  db_iter_ncols(const cutils_db_iter_t *iter);

/* Column name at idx, or NULL if idx is out of range. Pointer valid
 * for the iterator's lifetime. */
const char *db_iter_col_name(const cutils_db_iter_t *iter, int idx);

/* Nonzero if db_iter_next previously returned 0 due to a sqlite
 * step error rather than natural end-of-rows. The error message
 * was stashed in the thread-local error buffer at the time. */
int  db_iter_error(const cutils_db_iter_t *iter);

/* Finalize the statement, release the connection mutex, free the
 * iterator. Safe to call with NULL. */
void db_iter_end(cutils_db_iter_t *iter);

/* Pointer-cleanup helper. Use via CUTILS_AUTO_DB_ITER. */
void db_iter_end_p(cutils_db_iter_t **iter);

/* Scoped cleanup attribute. Place on the local variable holding the
 * iterator pointer; db_iter_end runs on any scope exit:
 *
 *   CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
 *   if (db_iter_begin(...) != CUTILS_OK) return -1;
 *   / * iterator released automatically on any return below * /
 */
#define CUTILS_AUTO_DB_ITER __attribute__((cleanup(db_iter_end_p)))

/* Savepoint management (used by migration runner) */
CUTILS_MUST_USE int db_savepoint         (cutils_db_t *db, const char *name);
CUTILS_MUST_USE int db_savepoint_release (cutils_db_t *db, const char *name);
CUTILS_MUST_USE int db_savepoint_rollback(cutils_db_t *db, const char *name);

/* --- Migration runner ---
 *
 * Migrations are tracked in a system_migrations table with SHA256 checksums.
 * Already-applied migrations are verified by checksum; a mismatch is a
 * fatal error. New migrations are applied within savepoints for rollback. */

/* Compiled migration entry. Terminate arrays with { NULL, NULL }. */
typedef struct {
    const char *name;  /* e.g. "001_users.sql" — used as tracking key */
    const char *sql;   /* full SQL text */
} db_migration_t;

/* Run compiled-in library migrations (_lib/ prefix).
 * Called internally by appguard_init(). */
CUTILS_MUST_USE
int db_run_lib_migrations(cutils_db_t *db);

/* Run file-based app migrations from a directory of numbered .sql files.
 * Files are applied in lexical order. NULL migrations_dir is a no-op. */
CUTILS_MUST_USE
int db_run_app_migrations(cutils_db_t *db, const char *migrations_dir);

/* Run compiled-in app migrations. The array must be NULL-terminated.
 * Names should match .sql filenames for compatibility with file-based tracking.
 * NULL migrations is a no-op. */
CUTILS_MUST_USE
int db_run_compiled_migrations(cutils_db_t *db, const db_migration_t *migrations);

#endif
