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
 */
typedef struct {
    cutils_db_t *db;
    int          active;     /* 1 if BEGIN succeeded */
    int          finalized;  /* 1 once commit or rollback has run */
} cutils_db_tx_t;

/* Begin a scoped transaction. Initializes *tx and issues BEGIN.
 * Returns CUTILS_OK on success; on failure, tx is left inactive and
 * the cleanup will be a no-op. */
CUTILS_MUST_USE
int cutils_db_tx_begin(cutils_db_t *db, cutils_db_tx_t *tx);

/* Commit a scoped transaction. On success, marks tx finalized so the
 * cleanup does not roll back. Idempotent. Returns CUTILS_OK or the
 * underlying db_commit error. */
CUTILS_MUST_USE
int db_tx_commit(cutils_db_tx_t *tx);

/* Cleanup helper invoked by CUTILS_AUTO_DB_TX.
 * Rolls back the transaction if it was begun and never finalized. */
void cutils_db_tx_end_p(cutils_db_tx_t *tx);

#define CUTILS_AUTO_DB_TX __attribute__((cleanup(cutils_db_tx_end_p)))

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
