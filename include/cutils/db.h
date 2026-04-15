#ifndef CUTILS_DB_H
#define CUTILS_DB_H

#include <stddef.h>
#include <stdint.h>

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
int db_open(cutils_db_t **db, const char *path);

/* Close and free the database handle. Safe to call with NULL. */
void db_close(cutils_db_t *db);

/* Execute a SELECT query. Caller must free the result with db_result_free().
 * params is a NULL-terminated array of string bind values, or NULL for no params.
 * Returns CUTILS_OK on success. On error, *result is set to NULL. */
int db_execute(cutils_db_t *db, const char *sql, const char **params,
               db_result_t **result);

/* Execute an INSERT/UPDATE/DELETE query. Returns CUTILS_OK on success.
 * If affected is non-NULL, stores the number of rows affected.
 * params is a NULL-terminated array of string bind values, or NULL. */
int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected);

/* Execute raw SQL (multiple statements, no params, no result).
 * Used internally by the migration runner. Returns CUTILS_OK on success. */
int db_exec_raw(cutils_db_t *db, const char *sql);

/* Free a query result set. Safe to call with NULL. */
void db_result_free(db_result_t *result);

/* Begin/commit/rollback transactions */
int db_begin(cutils_db_t *db);
int db_commit(cutils_db_t *db);
int db_rollback(cutils_db_t *db);

/* Savepoint management (used by migration runner) */
int db_savepoint(cutils_db_t *db, const char *name);
int db_savepoint_release(cutils_db_t *db, const char *name);
int db_savepoint_rollback(cutils_db_t *db, const char *name);

#endif
