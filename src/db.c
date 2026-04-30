#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cutils_db {
    sqlite3        *conn;
    pthread_mutex_t mutex;
};

/* File-local scoped cleanup for SQLite prepared statements. */
static void sqlite3_stmt_finalize_p(sqlite3_stmt **s)
{
    if (*s) {
        sqlite3_finalize(*s);
        *s = NULL;
    }
}
#define CUTILS_AUTO_STMT __attribute__((cleanup(sqlite3_stmt_finalize_p)))

/* --- Internal helpers --- */

static int bind_params(sqlite3_stmt *stmt, const char **params)
{
    if (!params) return CUTILS_OK;

    for (int i = 0; params[i] != NULL; i++) {
        if (sqlite3_bind_text(stmt, i + 1, params[i], -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            return set_error(CUTILS_ERR_DB, "bind param %d failed", i + 1);
        }
    }
    return CUTILS_OK;
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
    if (pthread_mutexattr_init(&mattr) != 0) {
        free(d);
        return set_error(CUTILS_ERR, "db_open: mutexattr init failed");
    }
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    int mrc = pthread_mutex_init(&d->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);
    if (mrc != 0) {
        free(d);
        return set_error(CUTILS_ERR, "db_open: mutex init failed");
    }

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

    *db = d;
    return CUTILS_OK;
}

void db_close(cutils_db_t *db)
{
    if (!db) return;
    pthread_mutex_lock(&db->mutex);
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = NULL;
    }
    pthread_mutex_unlock(&db->mutex);
    pthread_mutex_destroy(&db->mutex);
    free(db);
}

int db_execute(cutils_db_t *db, const char *sql, const char **params,
               db_result_t **result)
{
    *result = NULL;
    CUTILS_LOCK_GUARD(&db->mutex);
    CUTILS_AUTO_STMT sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return set_error(CUTILS_ERR_DB, "prepare: %s", sqlite3_errmsg(db->conn));

    rc = bind_params(stmt, params);
    if (rc != CUTILS_OK) return rc;

    int ncols = sqlite3_column_count(stmt);

    /* Collect rows into a dynamic array. rows and col_names are owned
     * locally until success, then transferred into the result struct. */
    int capacity = 16;
    int nrows    = 0;
    char ***rows      = calloc((size_t)capacity, sizeof(char **));
    char  **col_names = NULL;
    db_result_t *res  = NULL;
    int err           = CUTILS_OK;

    if (!rows) {
        err = set_error(CUTILS_ERR_NOMEM, "db_execute: row array alloc failed");
        goto fail;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (nrows >= capacity) {
            capacity *= 2;
            char ***tmp = realloc(rows, (size_t)capacity * sizeof(char **));
            if (!tmp) {
                err = set_error(CUTILS_ERR_NOMEM,
                                "db_execute: row realloc failed");
                goto fail;
            }
            rows = tmp;
        }

        char **row = calloc((size_t)ncols, sizeof(char *));
        if (!row) {
            err = set_error(CUTILS_ERR_NOMEM,
                            "db_execute: column alloc failed");
            goto fail;
        }

        for (int c = 0; c < ncols; c++) {
            const char *val = (const char *)sqlite3_column_text(stmt, c);
            row[c] = val ? strdup(val) : strdup("");
        }

        rows[nrows++] = row;
    }

    if (rc != SQLITE_DONE) {
        err = set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
        goto fail;
    }

    /* Build column names */
    col_names = calloc((size_t)ncols, sizeof(char *));
    if (col_names) {
        for (int c = 0; c < ncols; c++)
            col_names[c] = strdup(sqlite3_column_name(stmt, c));
    }

    res = calloc(1, sizeof(*res));
    if (!res) {
        err = set_error(CUTILS_ERR_NOMEM, "db_execute: result alloc failed");
        goto fail;
    }

    res->rows      = CUTILS_MOVE(rows);
    res->nrows     = nrows;
    res->ncols     = ncols;
    res->col_names = CUTILS_MOVE(col_names);
    *result = res;
    return CUTILS_OK;

fail:
    /* rows and col_names only non-NULL if they weren't moved into res. */
    if (rows) {
        for (int r = 0; r < nrows; r++) {
            if (rows[r]) {
                for (int c = 0; c < ncols; c++) free(rows[r][c]);
                free(rows[r]);
            }
        }
        free(rows);
    }
    if (col_names) {
        for (int c = 0; c < ncols; c++) free(col_names[c]);
        free(col_names);
    }
    return err;
}

int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected)
{
    CUTILS_LOCK_GUARD(&db->mutex);
    CUTILS_AUTO_STMT sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return set_error(CUTILS_ERR_DB, "prepare: %s", sqlite3_errmsg(db->conn));

    rc = bind_params(stmt, params);
    if (rc != CUTILS_OK) return rc;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        return set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));

    if (affected)
        *affected = sqlite3_changes(db->conn);

    return CUTILS_OK;
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

void db_result_free(db_result_t *result)
{
    if (!result) return;

    if (result->rows) {
        for (int r = 0; r < result->nrows; r++) {
            if (result->rows[r]) {
                for (int c = 0; c < result->ncols; c++)
                    free(result->rows[r][c]);
                free(result->rows[r]);
            }
        }
        free(result->rows);
    }

    if (result->col_names) {
        for (int c = 0; c < result->ncols; c++)
            free(result->col_names[c]);
        free(result->col_names);
    }

    free(result);
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

/* --- Scoped transaction guard ---
 *
 * The tx scope holds db->mutex from BEGIN through COMMIT or ROLLBACK so
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
    pthread_mutex_lock(&db->mutex);

    int rc = db_exec_raw(db, sql);
    if (rc != CUTILS_OK) {
        pthread_mutex_unlock(&db->mutex);
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
        pthread_mutex_unlock(&tx->db->mutex);
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
        pthread_mutex_unlock(&tx->db->mutex);
    }
    tx->active    = 0;
    tx->finalized = 1;
}
