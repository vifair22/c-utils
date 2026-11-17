#ifndef CUTILS_DB_H
#define CUTILS_DB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "cutils/mem.h"

/* --- Database subsystem ---
 *
 * Mutex-protected SQLite connection with WAL mode.
 * All access serialized through the mutex — safe to call from any thread.
 *
 * Query results are returned as a simple row/column structure that the
 * caller frees with db_result_free().
 *
 * Quick reference:
 *
 *   // Open / close
 *   cutils_db_t *db = NULL;
 *   db_open(&db, "/path/to/file.db");
 *   db_open_with_mode(&db, "/path/to/file.db", 0600);  // restrict perms
 *   db_close(db);
 *
 *   // SELECT (materialized, known-small result)
 *   CUTILS_AUTO_DBRES db_result_t *r = NULL;
 *   db_execute(db, "SELECT a, b FROM t WHERE c = ?", (const char*[]){ v, NULL }, &r);
 *   for (int i = 0; i < r->nrows; i++) use(r->rows[i][0]);
 *
 *   // SELECT (streaming, unbounded result)
 *   CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
 *   db_iter_begin(db, "SELECT a, b FROM t", NULL, &it);
 *   const char **row;
 *   while (db_iter_next(it, &row)) use(row[0]);
 *
 *   // INSERT / UPDATE / DELETE
 *   db_execute_non_query(db, "INSERT INTO t VALUES (?, ?)",
 *                        (const char*[]){ v1, v2, NULL }, NULL);
 *
 *   // Transaction (auto-rollback on early return)
 *   CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
 *   cutils_db_tx_begin(db, &tx);
 *   ... // work
 *   return db_tx_commit(&tx);
 */

/* Opaque database handle */
typedef struct cutils_db cutils_db_t;

/* Query result set */
typedef struct {
    char ***rows;      /* rows[row_idx][col_idx], NULL-terminated strings */
    int     nrows;
    int     ncols;
    char  **col_names; /* col_names[col_idx] */
} db_result_t;

/* Open a database connection. Creates the file if it doesn't exist
 * (subject to the calling process's umask).
 * Enables WAL mode and sets a 5-second busy timeout.
 * Returns CUTILS_OK on success, error code on failure. */
CUTILS_MUST_USE
int db_open(cutils_db_t **db, const char *path);

/* Open a database connection and chmod the file to the given mode.
 * Equivalent to db_open followed by an unconditional chmod — the mode
 * is enforced whether the file was just created or already existed,
 * so callers get an idempotent "the DB file has these permissions
 * after this call returns" guarantee.
 *
 * Intended for daemons whose DB holds sensitive data (credentials,
 * audit logs, etc.) and that don't want to rely on the calling
 * process's umask. Typical use:
 *
 *     db_open_with_mode(&db, "/var/lib/myapp.db", 0600);
 *
 * Note on WAL/SHM: sqlite creates path-wal and path-shm files lazily
 * on the first write transaction; their mode is determined by the
 * umask at that moment, NOT by this call. If you need strict
 * permissions on every artifact, set umask(0077) before this call so
 * subsequent WAL/SHM creates inherit the same restriction.
 *
 * Returns CUTILS_OK on success, CUTILS_ERR_IO on chmod failure (with
 * the connection torn down and *db set to NULL), or whatever db_open
 * returns on open failure. */
CUTILS_MUST_USE
int db_open_with_mode(cutils_db_t **db, const char *path, mode_t mode);

/* Close and free the database handle. Safe to call with NULL. */
void db_close(cutils_db_t *db);

/* Execute a SELECT query. Caller must free the result with db_result_free().
 * params is a NULL-terminated array of string bind values, or NULL for no params.
 * Returns CUTILS_OK on success. On error, *result is set to NULL. */
CUTILS_MUST_USE
int db_execute(cutils_db_t *db, const char *sql, const char **params,
               db_result_t **result);

/* Same as db_execute, but prepares a private statement that is
 * finalized on return instead of going through the statement cache.
 * Use for SQL that runs once (setup, ad-hoc admin queries, SQL built at
 * run time) so it does not push hot statements out of the cache. */
CUTILS_MUST_USE
int db_execute_once(cutils_db_t *db, const char *sql, const char **params,
                    db_result_t **result);

/* Execute an INSERT/UPDATE/DELETE query. Returns CUTILS_OK on success.
 * If affected is non-NULL, stores the number of rows affected.
 * params is a NULL-terminated array of string bind values, or NULL. */
CUTILS_MUST_USE
int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected);

/* db_execute_non_query without the statement cache; see db_execute_once. */
CUTILS_MUST_USE
int db_execute_non_query_once(cutils_db_t *db, const char *sql,
                              const char **params, int *affected);

/* --- Typed parameters ---
 *
 * The string-array API binds every value as TEXT and stops at the first
 * NULL, so it can bind neither raw bytes nor SQL NULL. The typed calls
 * take a counted array instead; a NULL pointer with nparams 0 means no
 * parameters. Values are copied at bind time, so the buffers they point
 * at only need to live for the call. Passing more params than the SQL
 * has placeholders is an error; passing fewer leaves the rest NULL.
 *
 *   uint8_t hash[32];
 *   db_param_t p[] = { DB_BLOB(hash, sizeof(hash)), DB_INT64(size) };
 *   db_execute_non_query_typed(db, "INSERT INTO f(hash, size) VALUES (?, ?)",
 *                              p, 2, NULL);
 *
 * Results are still text: select a BLOB column as lower(hex(col)). */
typedef enum {
    DB_P_NULL,
    DB_P_TEXT,
    DB_P_BLOB,
    DB_P_INT64,
    DB_P_DOUBLE,
} db_param_type_t;

typedef struct {
    db_param_type_t type;
    const void     *ptr;  /* TEXT: string; BLOB: bytes. NULL binds SQL NULL
                           * for either, whatever len says */
    size_t          len;  /* BLOB: byte count; TEXT: byte count, or 0 for
                           * NUL-terminated */
    int64_t         i64;
    double          dbl;
} db_param_t;

#define DB_TEXT(s)    ((db_param_t){ .type = DB_P_TEXT, .ptr = (s) })
#define DB_BLOB(p, n) ((db_param_t){ .type = DB_P_BLOB, .ptr = (p), .len = (n) })
#define DB_INT64(v)   ((db_param_t){ .type = DB_P_INT64, .i64 = (v) })
#define DB_DOUBLE(v)  ((db_param_t){ .type = DB_P_DOUBLE, .dbl = (v) })
#define DB_NULLP      ((db_param_t){ .type = DB_P_NULL })

/* db_execute with typed parameters. Uses the statement cache. */
CUTILS_MUST_USE
int db_execute_typed(cutils_db_t *db, const char *sql,
                     const db_param_t *params, size_t nparams,
                     db_result_t **result);

/* db_execute_non_query with typed parameters. Uses the statement cache. */
CUTILS_MUST_USE
int db_execute_non_query_typed(cutils_db_t *db, const char *sql,
                               const db_param_t *params, size_t nparams,
                               int *affected);

/* --- Prepared-statement cache ---
 *
 * db_execute, db_execute_non_query and their _typed forms keep prepared
 * statements per handle, keyed by the exact SQL text, in a bounded LRU
 * (CUTILS_DB_STMT_CACHE_DEFAULT entries unless changed). A repeat call
 * skips the parse and plan. Statements are reset after every call, so a
 * cached statement holds no read transaction and does not block WAL
 * checkpoints, and they are finalized by db_close. A schema change is
 * handled: sqlite re-prepares the statement on its next step.
 *
 * Key by text means "SELECT 1" and "SELECT  1" are two entries. Build
 * SQL once and reuse the string; SQL assembled at run time with varying
 * text (an IN list of varying length, for example) should go through
 * the _once calls instead, or it will churn the cache.
 *
 * db_exec_raw, the iterator and the migration runner do not use the
 * cache. */
#define CUTILS_DB_STMT_CACHE_DEFAULT 32
#define CUTILS_DB_STMT_CACHE_MAX     4096

typedef struct {
    uint64_t hits;        /* calls served by a cached statement */
    uint64_t misses;      /* calls that prepared a statement */
    uint64_t evictions;   /* entries dropped to make room */
    uint64_t bypasses;    /* calls that could not use the cache (entry
                           * busy in a nested call, or every entry busy) */
    uint64_t reprepares;  /* SQLITE_SCHEMA retries after sqlite's own */
    int      entries;     /* statements currently cached */
    int      capacity;    /* current size limit */
} db_stmt_cache_stats_t;

/* Set the cache capacity, 0 to disable caching on this handle. Shrinking
 * finalizes the least recently used statements. Returns CUTILS_OK, or
 * CUTILS_ERR_INVALID for a size outside 0..CUTILS_DB_STMT_CACHE_MAX. */
CUTILS_MUST_USE
int db_stmt_cache_set_size(cutils_db_t *db, int size);

/* Snapshot the cache counters. */
void db_stmt_cache_stats(cutils_db_t *db, db_stmt_cache_stats_t *out);

/* Execute raw SQL (multiple statements, no params, no result).
 * Used internally by the migration runner. Returns CUTILS_OK on success. */
CUTILS_MUST_USE
int db_exec_raw(cutils_db_t *db, const char *sql);

/* Nonzero if the cell at (row, col) of a result from db_execute or
 * db_execute_typed is SQL NULL. Such cells read as "" in rows[][], the
 * same as an empty string, so this is the only way to tell them apart.
 * Returns 0 for an out-of-range cell or a NULL result. */
int db_result_is_null(const db_result_t *result, int row, int col);

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

/* --- Application write lock (1.8.0) ---
 *
 * SQLite keeps one transaction per connection, not per thread. An app
 * that runs BEGIN ... COMMIT through db_exec_raw or db_begin, guarded
 * by its own lock, leaves the handle mutex free between statements, so
 * a statement another thread runs in that window lands inside the open
 * transaction and commits or rolls back with it. c-utils' own writers
 * (the log writer thread, push_send and the push worker) are such
 * threads, and they cannot see the app's lock.
 *
 * db_set_write_lock hands that lock to the handle. From then on every
 * c-utils write scope takes it first:
 *   - the log writer and log retention cleanup,
 *   - push_send's enqueue and the push worker's reads and updates,
 *   - cutils_db_tx_begin / cutils_db_tx_begin_immediate,
 *   - db_write_begin, for an app's own code.
 *
 * Contract for lock(ctx):
 *   - Block until the calling thread holds the lock, then return
 *     DB_WRITE_LOCK_ACQUIRED. unlock(ctx) is called once at scope end.
 *   - If the calling thread already holds it, return DB_WRITE_LOCK_HELD
 *     without blocking. unlock is NOT called. The write then joins the
 *     caller's open transaction, if it has one, which is what a
 *     push_send made inside it should do.
 *   - Return a negative value to refuse. The write fails with
 *     CUTILS_ERR_DB (the log writer drops the line).
 *
 * Lock order is the app's lock, then the handle mutex. Do not write
 * through c-utils from inside a db_iter scope while another thread may
 * hold the app lock: the iterator holds the handle mutex, so the two
 * threads would wait on each other.
 *
 * Install it before any thread starts a transaction with it, typically
 * right after db_open or appguard_init. Without a lock installed the
 * scopes take only the handle mutex, exactly as before 1.8.0. */
#define DB_WRITE_LOCK_ACQUIRED 0
#define DB_WRITE_LOCK_HELD     1

typedef int  (*db_write_lock_fn)(void *ctx);
typedef void (*db_write_unlock_fn)(void *ctx);

/* Install (or with both NULL, remove) the handle's write lock. Returns
 * CUTILS_OK, or CUTILS_ERR_INVALID if exactly one of lock and unlock is
 * NULL. */
CUTILS_MUST_USE
int db_set_write_lock(cutils_db_t *db, db_write_lock_fn lock,
                      db_write_unlock_fn unlock, void *ctx);

/* Nonzero if a write lock is installed on the handle. */
int db_has_write_lock(cutils_db_t *db);

/* A write scope: the app's write lock (if installed), then the handle
 * mutex, held until db_write_end. Statements run inside it cannot
 * interleave with another thread's transaction. Scopes nest on one
 * thread when the lock reports DB_WRITE_LOCK_HELD.
 *
 *   CUTILS_AUTO_DB_WRITE db_write_guard_t w = { 0 };
 *   if (db_write_begin(db, &w) != CUTILS_OK) return -1;
 *   ...statements...
 *   (released on scope exit) */
typedef struct {
    cutils_db_t        *db;
    db_write_unlock_fn  unlock;  /* NULL when there is nothing to release */
    void               *ctx;
    int                 active;  /* 1 while the scope is held */
} db_write_guard_t;

CUTILS_MUST_USE
int db_write_begin(cutils_db_t *db, db_write_guard_t *g);

/* Release a write scope. Idempotent; a no-op on an inactive guard. */
void db_write_end(db_write_guard_t *g);

#define CUTILS_AUTO_DB_WRITE __attribute__((cleanup(db_write_end)))

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
 * caller's committed state.
 *
 * Since 1.8.0 the scope is a db_write_begin scope: it takes the app's
 * write lock first when one is installed (see db_set_write_lock). */
typedef struct {
    cutils_db_t     *db;
    int              active;     /* 1 if BEGIN succeeded */
    int              finalized;  /* 1 once commit or rollback has run */
    db_write_guard_t wg;         /* write scope held from BEGIN to end */
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
