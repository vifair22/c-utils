#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- Prepared-statement cache ---
 *
 * Preparing a statement costs a parse and a plan, which for a short
 * INSERT or point SELECT is most of the call. Callers that run the same
 * SQL in a loop (one row per file in a backup, one sample per tick in a
 * daemon) were paying that on every call, so db_execute and
 * db_execute_non_query keep prepared statements per handle, keyed by
 * the exact SQL text.
 *
 * Structure: entries sit on a doubly linked LRU list (head = most
 * recently used) and in a chained hash table on the same nodes. Every
 * access is made under db->mutex, so the cache needs no lock of its
 * own. The bucket count is a power of two of at least twice the
 * capacity, which keeps chains short without rehashing on insert.
 *
 * Reuse: after each call the statement is sqlite3_reset, which ends its
 * read transaction (a statement left mid-step would pin the WAL
 * snapshot and starve checkpoints) but keeps its bindings.
 * sqlite3_clear_bindings is deliberately never called. Instead a
 * reused statement rebinds the parameters the caller passed and binds
 * NULL only to the trailing ones the previous call set and this one
 * did not, so an omitted parameter still reads as NULL, as it did when
 * every call prepared a fresh statement.
 *
 * Schema changes: a statement from sqlite3_prepare_v2/v3 re-prepares
 * itself inside sqlite3_step when the schema cookie moves, so a cached
 * statement survives ALTER TABLE and friends. If step still reports
 * SQLITE_SCHEMA (its internal retry budget ran out), the entry is
 * dropped and the call is re-prepared once from the SQL text. Any other
 * failure also drops the entry, so a statement in an unknown state is
 * never handed to the next caller.
 *
 * Re-entrancy: the handle mutex is recursive, so an application SQL
 * function running inside sqlite3_step can call db_execute on the same
 * handle. An entry that is checked out is never handed out again or
 * evicted; the nested call gets a private statement instead. */

#define STMT_CACHE_MIN_BUCKETS 16u

typedef struct stmt_ent {
    struct stmt_ent *prev;    /* LRU, toward the head (more recent) */
    struct stmt_ent *next;    /* LRU, toward the tail (less recent) */
    struct stmt_ent *hnext;   /* hash bucket chain */
    sqlite3_stmt    *stmt;
    uint64_t         hash;
    size_t           len;
    int              nbound;  /* parameters the last call bound */
    int              in_use;
    char             sql[];   /* the key, NUL-terminated */
} stmt_ent_t;

struct cutils_db {
    sqlite3        *conn;
    pthread_mutex_t mutex;

    stmt_ent_t    **buckets;
    size_t          nbuckets; /* power of two, or 0 before first use */
    stmt_ent_t     *lru_head;
    stmt_ent_t     *lru_tail;
    int             cache_cap;
    int             cache_count;
    db_stmt_cache_stats_t stats;

    /* Application write lock (1.8.0). Read and written under mutex. */
    db_write_lock_fn   wl_lock;
    db_write_unlock_fn wl_unlock;
    void              *wl_ctx;
};

/* A statement checked out for one call: either a cache entry or a
 * private statement finalized on release. */
typedef struct {
    sqlite3_stmt *stmt;
    stmt_ent_t   *ent;
    int           prev_nbound;
} stmt_lease_t;

/* Parameter source for one call: either the NULL-terminated string
 * array of the original API or a counted typed array. */
typedef struct {
    const char       **text;
    const db_param_t  *typed;
    size_t             ntyped;
} bind_src_t;

static uint64_t sql_hash(const char *s, size_t len)
{
    /* FNV-1a: one multiply per byte, and SQL texts are short. */
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t bucket_of(const cutils_db_t *db, uint64_t hash)
{
    return (size_t)(hash & (uint64_t)(db->nbuckets - 1));
}

static void lru_unlink(cutils_db_t *db, stmt_ent_t *e)
{
    if (e->prev) e->prev->next = e->next; else db->lru_head = e->next;
    if (e->next) e->next->prev = e->prev; else db->lru_tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_head(cutils_db_t *db, stmt_ent_t *e)
{
    e->prev = NULL;
    e->next = db->lru_head;
    if (db->lru_head) db->lru_head->prev = e;
    db->lru_head = e;
    if (!db->lru_tail) db->lru_tail = e;
}

static void cache_remove(cutils_db_t *db, stmt_ent_t *e)
{
    stmt_ent_t **pp = &db->buckets[bucket_of(db, e->hash)];
    while (*pp != e) pp = &(*pp)->hnext;
    *pp = e->hnext;
    lru_unlink(db, e);
    db->cache_count--;
    sqlite3_finalize(e->stmt);
    free(e);
}

/* Evict idle entries from the tail until count <= target. Entries that
 * are checked out are skipped; release drops them later if the cache
 * is still over capacity. */
static void cache_trim(cutils_db_t *db, int target)
{
    stmt_ent_t *e = db->lru_tail;
    while (e && db->cache_count > target) {
        stmt_ent_t *prev = e->prev;
        if (!e->in_use) {
            cache_remove(db, e);
            db->stats.evictions++;
        }
        e = prev;
    }
}

static void cache_free_all(cutils_db_t *db)
{
    stmt_ent_t *e = db->lru_head;
    while (e) {
        stmt_ent_t *next = e->next;
        sqlite3_finalize(e->stmt);
        free(e);
        e = next;
    }
    free(db->buckets);
    db->buckets     = NULL;
    db->nbuckets    = 0;
    db->lru_head    = db->lru_tail = NULL;
    db->cache_count = 0;
}

/* Size the bucket array for the current capacity and rehash. Returns
 * CUTILS_OK, or CUTILS_ERR_NOMEM with the old table left intact. */
static int cache_rebucket(cutils_db_t *db)
{
    size_t want = STMT_CACHE_MIN_BUCKETS;
    while (want < (size_t)db->cache_cap * 2u) want <<= 1;
    if (want == db->nbuckets) return CUTILS_OK;

    stmt_ent_t **nb = calloc(want, sizeof(*nb));
    /* LCOV_EXCL_START - a calloc of a few KiB failing is not reachable
     * without fault injection. */
    if (!nb)
        return set_error(CUTILS_ERR_NOMEM, "statement cache: bucket alloc");
    /* LCOV_EXCL_STOP */
    free(db->buckets);
    db->buckets  = nb;
    db->nbuckets = want;
    for (stmt_ent_t *e = db->lru_head; e; e = e->next) {
        size_t b = bucket_of(db, e->hash);
        e->hnext = nb[b];
        nb[b] = e;
    }
    return CUTILS_OK;
}

static int prepare_private(cutils_db_t *db, const char *sql,
                           stmt_lease_t *lease)
{
    if (sqlite3_prepare_v2(db->conn, sql, -1, &lease->stmt, NULL) != SQLITE_OK) {
        lease->stmt = NULL;
        return set_error(CUTILS_ERR_DB, "prepare: %s", sqlite3_errmsg(db->conn));
    }
    return CUTILS_OK;
}

/* Check out a statement for sql. With use_cache set and a nonzero
 * capacity, returns a cached statement (preparing and inserting it on a
 * miss); otherwise, or when the cached copy is already checked out or
 * every entry is busy, a private one. Caller holds db->mutex. */
static int stmt_acquire(cutils_db_t *db, const char *sql, int use_cache,
                        stmt_lease_t *lease)
{
    lease->stmt        = NULL;
    lease->ent         = NULL;
    lease->prev_nbound = 0;

    if (!use_cache || db->cache_cap == 0)
        return prepare_private(db, sql, lease);

    if (!db->buckets && cache_rebucket(db) != CUTILS_OK)
        return prepare_private(db, sql, lease); /* LCOV_EXCL_LINE */

    size_t   len = strlen(sql);
    uint64_t h   = sql_hash(sql, len);
    size_t   b   = bucket_of(db, h);

    for (stmt_ent_t *e = db->buckets[b]; e; e = e->hnext) {
        if (e->hash != h || e->len != len || memcmp(e->sql, sql, len) != 0)
            continue;
        /* LCOV_EXCL_START - only reachable from a nested call made
         * inside sqlite3_step (an application SQL function), which the
         * public API gives no way to register on this connection. */
        if (e->in_use) {
            db->stats.bypasses++;
            return prepare_private(db, sql, lease);
        }
        /* LCOV_EXCL_STOP */
        db->stats.hits++;
        if (db->lru_head != e) {
            lru_unlink(db, e);
            lru_push_head(db, e);
        }
        e->in_use          = 1;
        lease->stmt        = e->stmt;
        lease->ent         = e;
        lease->prev_nbound = e->nbound;
        return CUTILS_OK;
    }

    db->stats.misses++;
    if (db->cache_count >= db->cache_cap)
        cache_trim(db, db->cache_cap - 1);
    /* LCOV_EXCL_START - every entry busy; same nested-call case. */
    if (db->cache_count >= db->cache_cap) {
        db->stats.bypasses++;
        return prepare_private(db, sql, lease);
    }
    /* LCOV_EXCL_STOP */

    stmt_ent_t *e = malloc(sizeof(*e) + len + 1);
    /* LCOV_EXCL_START - small malloc failure; the call still works,
     * it just is not cached. */
    if (!e)
        return prepare_private(db, sql, lease);
    /* LCOV_EXCL_STOP */

    /* PERSISTENT tells sqlite the statement is long-lived, so it keeps
     * it out of the connection's lookaside allocator, which is meant
     * for short-lived objects. */
    if (sqlite3_prepare_v3(db->conn, sql, (int)len, SQLITE_PREPARE_PERSISTENT,
                           &e->stmt, NULL) != SQLITE_OK) {
        int err = set_error(CUTILS_ERR_DB, "prepare: %s",
                            sqlite3_errmsg(db->conn));
        sqlite3_finalize(e->stmt);
        free(e);
        return err;
    }
    memcpy(e->sql, sql, len + 1);
    e->hash   = h;
    e->len    = len;
    e->nbound = 0;
    e->in_use = 1;
    e->hnext  = db->buckets[b];
    db->buckets[b] = e;
    lru_push_head(db, e);
    db->cache_count++;

    lease->stmt = e->stmt;
    lease->ent  = e;
    return CUTILS_OK;
}

/* Return a statement checked out by stmt_acquire. failed drops a cached
 * entry instead of keeping it. Caller holds db->mutex. */
static void stmt_release(cutils_db_t *db, stmt_lease_t *lease, int failed)
{
    if (!lease->stmt) return;
    if (!lease->ent) {
        sqlite3_finalize(lease->stmt);
    } else {
        stmt_ent_t *e = lease->ent;
        sqlite3_reset(e->stmt);
        e->in_use = 0;
        if (failed || db->cache_count > db->cache_cap)
            cache_remove(db, e);
    }
    lease->stmt = NULL;
    lease->ent  = NULL;
}

static int bind_one_typed(sqlite3_stmt *stmt, int idx, const db_param_t *p)
{
    switch (p->type) {
    case DB_P_NULL:
        return sqlite3_bind_null(stmt, idx);
    case DB_P_TEXT:
        if (!p->ptr) return sqlite3_bind_null(stmt, idx);
        if (p->len > (size_t)INT32_MAX) return SQLITE_TOOBIG;
        return sqlite3_bind_text(stmt, idx, (const char *)p->ptr,
                                 p->len ? (int)p->len : -1, SQLITE_TRANSIENT);
    case DB_P_BLOB:
        /* A NULL pointer binds SQL NULL whatever len says, the same rule
         * as DB_TEXT, so DB_BLOB(have ? hash : NULL, 32) means "hash or
         * NULL". 1.7.0 swapped in a 1-byte "" and kept len, so a NULL
         * pointer with len 32 read 31 bytes past it. */
        if (!p->ptr) return sqlite3_bind_null(stmt, idx);
        if (p->len > (size_t)INT32_MAX) return SQLITE_TOOBIG;
        return sqlite3_bind_blob(stmt, idx, p->ptr, (int)p->len,
                                 SQLITE_TRANSIENT);
    case DB_P_INT64:
        return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)p->i64);
    case DB_P_DOUBLE:
        return sqlite3_bind_double(stmt, idx, p->dbl);
    }
    return SQLITE_MISUSE;
}

/* Bind src to the leased statement, then NULL any trailing parameters
 * the previous user of a cached statement bound and this call did not
 * (see the cache comment above for why not sqlite3_clear_bindings). */
static int bind_lease(stmt_lease_t *lease, const bind_src_t *src)
{
    sqlite3_stmt *stmt = lease->stmt;
    int n = 0;

    if (src->text) {
        for (; src->text[n] != NULL; n++) {
            if (sqlite3_bind_text(stmt, n + 1, src->text[n], -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK)
                return set_error(CUTILS_ERR_DB, "bind param %d failed", n + 1);
        }
    } else if (src->typed) {
        if (src->ntyped > (size_t)sqlite3_bind_parameter_count(stmt))
            return set_error(CUTILS_ERR_DB,
                             "bind: %zu params for %d placeholders",
                             src->ntyped, sqlite3_bind_parameter_count(stmt));
        for (; (size_t)n < src->ntyped; n++) {
            if (bind_one_typed(stmt, n + 1, &src->typed[n]) != SQLITE_OK)
                return set_error(CUTILS_ERR_DB, "bind param %d failed", n + 1);
        }
    }

    for (int i = n; i < lease->prev_nbound; i++)
        sqlite3_bind_null(stmt, i + 1);
    if (lease->ent) lease->ent->nbound = n;
    return CUTILS_OK;
}

/* Acquire, bind and take the first step. On success *lease is checked
 * out and *first_rc is SQLITE_ROW or SQLITE_DONE; on failure nothing is
 * held and the error is set. The SQLITE_SCHEMA retry lives here because
 * a schema error can only come from the first step of a statement. */
static int stmt_start(cutils_db_t *db, const char *sql, const bind_src_t *src,
                      int use_cache, stmt_lease_t *lease, int *first_rc)
{
    for (int attempt = 0; ; attempt++) {
        int rc = stmt_acquire(db, sql, use_cache, lease);
        if (rc != CUTILS_OK) return rc;

        rc = bind_lease(lease, src);
        if (rc != CUTILS_OK) {
            stmt_release(db, lease, 1);
            return rc;
        }

        int step = sqlite3_step(lease->stmt);
        if (step == SQLITE_ROW || step == SQLITE_DONE) {
            *first_rc = step;
            return CUTILS_OK;
        }

        /* LCOV_EXCL_START - prepare_v2/v3 statements already retry a
         * schema change inside sqlite3_step; reaching SQLITE_SCHEMA here
         * takes the schema changing on every one of sqlite's internal
         * retries, which a test cannot arrange deterministically. */
        if (step == SQLITE_SCHEMA && attempt == 0) {
            stmt_release(db, lease, 1);
            db->stats.reprepares++;
            continue;
        }
        /* LCOV_EXCL_STOP */

        rc = set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
        stmt_release(db, lease, 1);
        return rc;
    }
}

/* --- Public API --- */

int db_open(cutils_db_t **db, const char *path)
{
    cutils_db_t *d = calloc(1, sizeof(*d));
    if (!d)
        return set_error(CUTILS_ERR_NOMEM, "db_open: allocation failed");

    /* Recursive so a thread that holds the mutex across a tx scope (see
     * cutils_db_tx_begin) can still call db_execute or db_exec_raw inside
     * that scope without self-deadlocking on the per-call lock guard. */
    pthread_mutexattr_t mattr;
    /* LCOV_EXCL_START — pthread_mutexattr_init / pthread_mutex_init only
     * fail on EAGAIN (resource exhaustion) or EINVAL (bad attr); neither
     * is reachable from this call site. Excluded from coverage so a
     * truly-unreachable failure path doesn't drag the line ratio down. */
    if (pthread_mutexattr_init(&mattr) != 0) {
        free(d);
        return set_error(CUTILS_ERR, "db_open: mutexattr init failed");
    }
    /* LCOV_EXCL_STOP */
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    int mrc = pthread_mutex_init(&d->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);
    /* LCOV_EXCL_START */
    if (mrc != 0) {
        free(d);
        return set_error(CUTILS_ERR, "db_open: mutex init failed");
    }
    /* LCOV_EXCL_STOP */

    int rc = sqlite3_open(path, &d->conn);
    if (rc != SQLITE_OK) {
        const char *msg = d->conn ? sqlite3_errmsg(d->conn) : "unknown";
        int err = set_error(CUTILS_ERR_DB, "db_open(%s): %s", path, msg);
        if (d->conn) sqlite3_close(d->conn);
        pthread_mutex_destroy(&d->mutex);
        free(d);
        return err;
    }

    /* WAL mode for concurrent read/write */
    char *errmsg = NULL;
    sqlite3_exec(d->conn, "PRAGMA journal_mode=WAL;", NULL, NULL, &errmsg);
    if (errmsg) sqlite3_free(errmsg);

    /* 5 second busy timeout */
    sqlite3_busy_timeout(d->conn, 5000);

    d->cache_cap = CUTILS_DB_STMT_CACHE_DEFAULT;

    *db = d;
    return CUTILS_OK;
}

int db_open_with_mode(cutils_db_t **db, const char *path, mode_t mode)
{
    int rc = db_open(db, path);
    if (rc != CUTILS_OK) return rc;

    /* Unconditional chmod: makes the API idempotent — the caller is
     * guaranteed `the file has mode X` regardless of whether it was
     * just-created (subject to umask) or already existed. The
     * WAL/SHM sidecars are NOT touched here; if strict perms are
     * needed on those too, the caller should set umask(0077) before
     * this call so sqlite's lazy WAL/SHM creates inherit. */
    if (chmod(path, mode) != 0) {
        /* LCOV_EXCL_START — chmod on a just-opened file we own
         * shouldn't fail; exercising this needs an EROFS mount or
         * similar deliberate harness. */
        int err = set_error_errno(CUTILS_ERR_IO, "chmod %s", path);
        db_close(*db);
        *db = NULL;
        return err;
        /* LCOV_EXCL_STOP */
    }
    return CUTILS_OK;
}

void db_close(cutils_db_t *db)
{
    if (!db) return;
    pthread_mutex_lock(&db->mutex);
    /* sqlite3_close refuses (SQLITE_BUSY) while any statement is
     * unfinalized, which would leak the connection, so the cache goes
     * first. */
    cache_free_all(db);
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = NULL;
    }
    pthread_mutex_unlock(&db->mutex);
    pthread_mutex_destroy(&db->mutex);
    free(db);
}

/* db_result_t is public, so the per-cell NULL flags live in a private
 * wrapper around it; db_result_free and db_result_is_null recover the
 * wrapper from the public pointer. Every result this file hands out is
 * allocated this way. */
typedef struct {
    db_result_t    pub;
    unsigned char *nulls; /* nrows * ncols flags, row-major */
} result_box_t;

static result_box_t *box_of(const db_result_t *r)
{
    return (result_box_t *)((char *)(uintptr_t)r - offsetof(result_box_t, pub));
}

static void free_rows(char ***rows, int nrows, int ncols)
{
    if (!rows) return;
    for (int r = 0; r < nrows; r++) {
        if (rows[r]) {
            for (int c = 0; c < ncols; c++) free(rows[r][c]);
            free(rows[r]);
        }
    }
    free(rows);
}

static int run_query(cutils_db_t *db, const char *sql, const bind_src_t *src,
                     int use_cache, db_result_t **result)
{
    *result = NULL;
    CUTILS_LOCK_GUARD(&db->mutex);

    stmt_lease_t lease;
    int rc;
    int err = stmt_start(db, sql, src, use_cache, &lease, &rc);
    if (err != CUTILS_OK) return err;

    /* Read after the first step, not after prepare: a cached statement
     * that sqlite re-prepared for a schema change inside that step can
     * have a different column count (SELECT * after ALTER TABLE). */
    int ncols = sqlite3_column_count(lease.stmt);

    int capacity = 16;
    int nrows    = 0;
    char ***rows        = calloc((size_t)capacity, sizeof(char **));
    unsigned char *nulls = calloc((size_t)capacity * (size_t)(ncols ? ncols : 1), 1);
    char  **col_names   = NULL;
    result_box_t *box   = NULL;

    if (!rows || !nulls) {
        err = set_error(CUTILS_ERR_NOMEM, "db_execute: row array alloc failed");
        goto fail;
    }

    for (; rc == SQLITE_ROW; rc = sqlite3_step(lease.stmt)) {
        if (nrows >= capacity) {
            capacity *= 2;
            char ***tmp = realloc(rows, (size_t)capacity * sizeof(char **));
            if (!tmp) {
                err = set_error(CUTILS_ERR_NOMEM,
                                "db_execute: row realloc failed");
                goto fail;
            }
            rows = tmp;
            unsigned char *ntmp = realloc(nulls,
                (size_t)capacity * (size_t)(ncols ? ncols : 1));
            if (!ntmp) {
                err = set_error(CUTILS_ERR_NOMEM,
                                "db_execute: null map realloc failed");
                goto fail;
            }
            nulls = ntmp;
        }

        char **row = calloc((size_t)(ncols ? ncols : 1), sizeof(char *));
        if (!row) {
            err = set_error(CUTILS_ERR_NOMEM,
                            "db_execute: column alloc failed");
            goto fail;
        }

        unsigned char *rnull = nulls + (size_t)nrows * (size_t)ncols;
        for (int c = 0; c < ncols; c++) {
            /* column_text returns NULL only for SQL NULL (a zero-length
             * BLOB or TEXT comes back as ""), or when sqlite is out of
             * memory. Testing the pointer instead of calling
             * sqlite3_column_type saves ~10 ns per cell, which was a 19%
             * slowdown on db_execute_1000_rows. */
            const char *val = (const char *)sqlite3_column_text(lease.stmt, c);
            int is_null = val == NULL;
            rnull[c] = (unsigned char)is_null;
            row[c] = val ? strdup(val) : strdup("");
        }

        rows[nrows++] = row;
    }

    if (rc != SQLITE_DONE) {
        err = set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
        goto fail;
    }

    col_names = calloc((size_t)(ncols ? ncols : 1), sizeof(char *));
    if (col_names) {
        for (int c = 0; c < ncols; c++)
            col_names[c] = strdup(sqlite3_column_name(lease.stmt, c));
    }

    box = calloc(1, sizeof(*box));
    if (!box) {
        err = set_error(CUTILS_ERR_NOMEM, "db_execute: result alloc failed");
        goto fail;
    }

    stmt_release(db, &lease, 0);
    box->pub.rows      = CUTILS_MOVE(rows);
    box->pub.nrows     = nrows;
    box->pub.ncols     = ncols;
    box->pub.col_names = CUTILS_MOVE(col_names);
    box->nulls         = CUTILS_MOVE(nulls);
    *result = &box->pub;
    return CUTILS_OK;

fail:
    stmt_release(db, &lease, 1);
    free_rows(rows, nrows, ncols);
    free(nulls);
    if (col_names) {
        for (int c = 0; c < ncols; c++) free(col_names[c]);
        free(col_names);
    }
    return err;
}

static int run_non_query(cutils_db_t *db, const char *sql,
                         const bind_src_t *src, int use_cache, int *affected)
{
    CUTILS_LOCK_GUARD(&db->mutex);

    stmt_lease_t lease;
    int rc;
    int err = stmt_start(db, sql, src, use_cache, &lease, &rc);
    if (err != CUTILS_OK) return err;

    if (rc != SQLITE_DONE) {
        err = set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
        stmt_release(db, &lease, 1);
        return err;
    }

    if (affected)
        *affected = sqlite3_changes(db->conn);
    stmt_release(db, &lease, 0);
    return CUTILS_OK;
}

int db_execute(cutils_db_t *db, const char *sql, const char **params,
               db_result_t **result)
{
    bind_src_t src = { .text = params };
    return run_query(db, sql, &src, 1, result);
}

int db_execute_once(cutils_db_t *db, const char *sql, const char **params,
                    db_result_t **result)
{
    bind_src_t src = { .text = params };
    return run_query(db, sql, &src, 0, result);
}

int db_execute_typed(cutils_db_t *db, const char *sql,
                     const db_param_t *params, size_t nparams,
                     db_result_t **result)
{
    bind_src_t src = { .typed = params, .ntyped = params ? nparams : 0 };
    return run_query(db, sql, &src, 1, result);
}

int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected)
{
    bind_src_t src = { .text = params };
    return run_non_query(db, sql, &src, 1, affected);
}

int db_execute_non_query_once(cutils_db_t *db, const char *sql,
                              const char **params, int *affected)
{
    bind_src_t src = { .text = params };
    return run_non_query(db, sql, &src, 0, affected);
}

int db_execute_non_query_typed(cutils_db_t *db, const char *sql,
                               const db_param_t *params, size_t nparams,
                               int *affected)
{
    bind_src_t src = { .typed = params, .ntyped = params ? nparams : 0 };
    return run_non_query(db, sql, &src, 1, affected);
}

int db_stmt_cache_set_size(cutils_db_t *db, int size)
{
    if (size < 0 || size > CUTILS_DB_STMT_CACHE_MAX)
        return set_error(CUTILS_ERR_INVALID,
                         "statement cache size %d out of range 0..%d",
                         size, CUTILS_DB_STMT_CACHE_MAX);
    CUTILS_LOCK_GUARD(&db->mutex);
    int old = db->cache_cap;
    db->cache_cap = size;
    cache_trim(db, size);
    if (db->buckets && size > 0 && cache_rebucket(db) != CUTILS_OK) {
        /* LCOV_EXCL_START - bucket calloc failure */
        db->cache_cap = old < size ? old : size;
        return CUTILS_ERR_NOMEM;
        /* LCOV_EXCL_STOP */
    }
    return CUTILS_OK;
}

void db_stmt_cache_stats(cutils_db_t *db, db_stmt_cache_stats_t *out)
{
    CUTILS_LOCK_GUARD(&db->mutex);
    *out = db->stats;
    out->entries  = db->cache_count;
    out->capacity = db->cache_cap;
}

int db_exec_raw(cutils_db_t *db, const char *sql)
{
    CUTILS_LOCK_GUARD(&db->mutex);

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        int err = set_error(CUTILS_ERR_DB, "exec: %s",
                            errmsg ? errmsg : "unknown");
        if (errmsg) sqlite3_free(errmsg);
        return err;
    }

    return CUTILS_OK;
}

int db_result_is_null(const db_result_t *result, int row, int col)
{
    if (!result || row < 0 || row >= result->nrows ||
        col < 0 || col >= result->ncols)
        return 0;
    const result_box_t *box = box_of(result);
    return box->nulls[(size_t)row * (size_t)result->ncols + (size_t)col];
}

void db_result_free(db_result_t *result)
{
    if (!result) return;

    free_rows(result->rows, result->nrows, result->ncols);

    if (result->col_names) {
        for (int c = 0; c < result->ncols; c++)
            free(result->col_names[c]);
        free(result->col_names);
    }

    result_box_t *box = box_of(result);
    free(box->nulls);
    free(box);
}

void db_result_free_p(db_result_t **result)
{
    if (*result) {
        db_result_free(*result);
        *result = NULL;
    }
}

/* --- Transactions --- */

int db_begin(cutils_db_t *db)
{
    return db_exec_raw(db, "BEGIN");
}

int db_commit(cutils_db_t *db)
{
    return db_exec_raw(db, "COMMIT");
}

int db_rollback(cutils_db_t *db)
{
    return db_exec_raw(db, "ROLLBACK");
}

/* --- Savepoints --- */

int db_savepoint(cutils_db_t *db, const char *name)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SAVEPOINT %s", name);
    return db_exec_raw(db, sql);
}

int db_savepoint_release(cutils_db_t *db, const char *name)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT %s", name);
    return db_exec_raw(db, sql);
}

int db_savepoint_rollback(cutils_db_t *db, const char *name)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "ROLLBACK TO SAVEPOINT %s", name);
    return db_exec_raw(db, sql);
}

/* --- Application write lock (1.8.0) ---
 *
 * The hook is read under the handle mutex into the guard, then the
 * mutex is dropped before calling it: the app's lock must be taken
 * before the handle mutex, never while holding it, or a thread inside
 * the app's transaction (holding the app lock, waiting on the mutex for
 * its next statement) and a writer here would deadlock. The guard keeps
 * its own copy of unlock/ctx so a scope releases what it took even if
 * the hook were changed meanwhile. */

int db_set_write_lock(cutils_db_t *db, db_write_lock_fn lock,
                      db_write_unlock_fn unlock, void *ctx)
{
    if (!db || (!lock) != (!unlock))
        return set_error(CUTILS_ERR_INVALID,
                         "db_set_write_lock: lock and unlock must both be set or both NULL");
    pthread_mutex_lock(&db->mutex);
    db->wl_lock   = lock;
    db->wl_unlock = unlock;
    db->wl_ctx    = lock ? ctx : NULL;
    pthread_mutex_unlock(&db->mutex);
    return CUTILS_OK;
}

int db_has_write_lock(cutils_db_t *db)
{
    if (!db) return 0;
    pthread_mutex_lock(&db->mutex);
    int has = db->wl_lock != NULL;
    pthread_mutex_unlock(&db->mutex);
    return has;
}

int db_write_begin(cutils_db_t *db, db_write_guard_t *g)
{
    if (!g) return set_error(CUTILS_ERR_INVALID, "db_write_begin: null guard");
    g->db     = db;
    g->unlock = NULL;
    g->ctx    = NULL;
    g->active = 0;
    if (!db) return set_error(CUTILS_ERR_INVALID, "db_write_begin: null handle");

    pthread_mutex_lock(&db->mutex);
    db_write_lock_fn   lock   = db->wl_lock;
    db_write_unlock_fn unlock = db->wl_unlock;
    void              *ctx    = db->wl_ctx;
    pthread_mutex_unlock(&db->mutex);

    if (lock) {
        int lrc = lock(ctx);
        if (lrc < 0)
            return set_error(CUTILS_ERR_DB, "db_write_begin: write lock refused (%d)", lrc);
        if (lrc == DB_WRITE_LOCK_ACQUIRED) {
            g->unlock = unlock;
            g->ctx    = ctx;
        }
    }

    pthread_mutex_lock(&db->mutex);
    g->active = 1;
    return CUTILS_OK;
}

void db_write_end(db_write_guard_t *g)
{
    if (!g || !g->active) return;
    g->active = 0;
    pthread_mutex_unlock(&g->db->mutex);
    if (g->unlock) {
        db_write_unlock_fn unlock = g->unlock;
        g->unlock = NULL;
        unlock(g->ctx);
    }
}

/* --- Scoped transaction guard ---
 *
 * The tx scope is a write scope (db_write_begin): the app's write lock
 * when one is installed, then db->mutex, held from BEGIN through COMMIT
 * or ROLLBACK so
 * that no other thread can interleave its queries into the active
 * transaction (SQLite tracks transactions per-connection — without the
 * extended lock hold, a parallel writer's INSERT would land inside this
 * thread's tx and be rolled back on early return).
 *
 * The mutex is recursive (see db_open), so db_execute and db_exec_raw
 * calls inside the scope re-enter the lock counter cleanly. */

static int tx_begin_with_sql(cutils_db_t *db, cutils_db_tx_t *tx,
                             const char *sql)
{
    tx->db        = db;
    tx->active    = 0;
    tx->finalized = 0;

    /* Acquire the +1 hold that spans the whole tx. db_exec_raw below
     * takes its own +1 internally and releases on return, leaving our
     * hold in place. */
    int rc = db_write_begin(db, &tx->wg);
    if (rc != CUTILS_OK) return rc;

    rc = db_exec_raw(db, sql);
    if (rc != CUTILS_OK) {
        db_write_end(&tx->wg);
        return rc;
    }

    tx->active = 1;
    return CUTILS_OK;
}

int cutils_db_tx_begin(cutils_db_t *db, cutils_db_tx_t *tx)
{
    return tx_begin_with_sql(db, tx, "BEGIN");
}

int cutils_db_tx_begin_immediate(cutils_db_t *db, cutils_db_tx_t *tx)
{
    return tx_begin_with_sql(db, tx, "BEGIN IMMEDIATE");
}

int db_tx_commit(cutils_db_tx_t *tx)
{
    if (!tx || !tx->active || tx->finalized)
        return CUTILS_OK;

    int rc = db_commit(tx->db);
    if (rc == CUTILS_OK) {
        tx->finalized = 1;
        /* Release the hold acquired in tx_begin_with_sql. On commit
         * failure we leave it held so cutils_db_tx_end_p can still
         * issue a rollback under the same exclusive scope. */
        db_write_end(&tx->wg);
    }
    return rc;
}

void cutils_db_tx_end_p(cutils_db_tx_t *tx)
{
    if (!tx) return;
    if (tx->active && !tx->finalized) {
        /* Rollback can fail (e.g. DB closed underneath us); not much we
         * can do in a cleanup handler. Silently attempt and move on. */
        CUTILS_UNUSED(db_rollback(tx->db));
        /* Always release the hold we took in tx_begin_with_sql, even if
         * rollback failed — leaving the mutex locked would wedge every
         * other thread on this connection. */
        db_write_end(&tx->wg);
    }
    tx->active    = 0;
    tx->finalized = 1;
}

/* --- Streaming query iterator ---
 *
 * The iterator holds the connection mutex across its lifetime so the
 * prepared statement and sqlite's internal column buffers remain
 * stable between successive db_iter_next calls. The recursive mutex
 * (see db_open) means an iterator can be opened from inside a tx
 * scope — both hold the mutex, both release on close.
 *
 * Row pointers handed out via db_iter_next point into sqlite's
 * column-text storage, which sqlite recycles on the next step. The
 * row_buf array itself is allocated once at db_iter_begin and reused
 * across every next() call — caller-visible pointers are stable, the
 * strings they reference are not. */

struct cutils_db_iter {
    cutils_db_t   *db;
    sqlite3_stmt  *stmt;
    int            ncols;
    const char   **row_buf;     /* ncols slots, refilled per step */
    char         **col_names;   /* ncols strdup'd names */
    int            holds_mutex; /* did we acquire db->mutex? */
    int            errored;     /* step returned non-DONE/ROW */
};

/* Iterators prepare a private statement rather than use the cache:
 * db_iter_ncols and the column names are read before the first step,
 * so a cached statement that sqlite re-prepares for a schema change
 * during that step could change shape under them. An iterator also
 * amortizes its one prepare over every row it streams. */
int db_iter_begin(cutils_db_t *db, const char *sql,
                  const char **params, cutils_db_iter_t **out)
{
    *out = NULL;

    /* LCOV_EXCL_START — calloc(<48 bytes>, 1) failure is unreachable in
     * any practical test environment. Excluded so a fundamentally
     * untestable path doesn't drag the line ratio down. */
    cutils_db_iter_t *it = calloc(1, sizeof(*it));
    if (!it)
        return set_error(CUTILS_ERR_NOMEM, "db_iter_begin: alloc failed");
    /* LCOV_EXCL_STOP */
    it->db = db;

    pthread_mutex_lock(&db->mutex);
    it->holds_mutex = 1;

    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &it->stmt, NULL);
    if (rc != SQLITE_OK) {
        rc = set_error(CUTILS_ERR_DB, "prepare: %s",
                       sqlite3_errmsg(db->conn));
        goto fail;
    }

    {
        stmt_lease_t lease = { .stmt = it->stmt };
        bind_src_t   src   = { .text = params };
        rc = bind_lease(&lease, &src);
    }
    if (rc != CUTILS_OK)
        goto fail;

    it->ncols = sqlite3_column_count(it->stmt);
    if (it->ncols > 0) {
        it->row_buf   = calloc((size_t)it->ncols, sizeof(*it->row_buf));
        it->col_names = calloc((size_t)it->ncols, sizeof(*it->col_names));
        /* LCOV_EXCL_START — alloc failure on a few hundred bytes is
         * not exercisable from test code without fault injection. The
         * fail-path is reached via the same goto and db_iter_end
         * tolerates partial state (NULL slots, no-stmt, etc.). */
        if (!it->row_buf || !it->col_names) {
            rc = set_error(CUTILS_ERR_NOMEM,
                           "db_iter_begin: row/colname alloc");
            goto fail;
        }
        /* LCOV_EXCL_STOP */
        for (int c = 0; c < it->ncols; c++) {
            it->col_names[c] = strdup(sqlite3_column_name(it->stmt, c));
            /* LCOV_EXCL_START — same rationale: small strdup failure
             * not reachable in tests. db_iter_end frees the partial
             * col_names array (NULL slots are no-op'd by free). */
            if (!it->col_names[c]) {
                rc = set_error(CUTILS_ERR_NOMEM,
                               "db_iter_begin: col name alloc");
                goto fail;
            }
            /* LCOV_EXCL_STOP */
        }
    }

    *out = it;
    return CUTILS_OK;

fail:
    /* db_iter_end handles every shape of partial state: stmt may be
     * NULL (prepare failed), col_names may be partially populated
     * (strdup-loop bail), and holds_mutex tracks whether to unlock. */
    db_iter_end(it);
    return rc;
}

int db_iter_next(cutils_db_iter_t *it, const char ***row_out)
{
    if (!it || it->errored) return 0;

    int rc = sqlite3_step(it->stmt);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_ROW) {
        CUTILS_UNUSED(set_error(CUTILS_ERR_DB, "step: %s",
                                sqlite3_errmsg(it->db->conn)));
        it->errored = 1;
        return 0;
    }

    for (int c = 0; c < it->ncols; c++) {
        const char *v = (const char *)sqlite3_column_text(it->stmt, c);
        /* NULL columns → "" to match db_execute's contract. The empty
         * string literal lives in static storage; safe to return as a
         * const char *. */
        it->row_buf[c] = v ? v : "";
    }

    if (row_out) *row_out = it->row_buf;
    return 1;
}

int db_iter_ncols(const cutils_db_iter_t *it)
{
    return it ? it->ncols : 0;
}

const char *db_iter_col_name(const cutils_db_iter_t *it, int idx)
{
    if (!it || idx < 0 || idx >= it->ncols) return NULL;
    return it->col_names[idx];
}

int db_iter_error(const cutils_db_iter_t *it)
{
    return it ? it->errored : 0;
}

void db_iter_end(cutils_db_iter_t *it)
{
    if (!it) return;
    if (it->stmt) sqlite3_finalize(it->stmt);
    if (it->col_names) {
        for (int c = 0; c < it->ncols; c++) free(it->col_names[c]);
        free(it->col_names);
    }
    free(it->row_buf);
    if (it->holds_mutex) pthread_mutex_unlock(&it->db->mutex);
    free(it);
}

void db_iter_end_p(cutils_db_iter_t **it)
{
    if (it && *it) {
        db_iter_end(*it);
        *it = NULL;
    }
}
