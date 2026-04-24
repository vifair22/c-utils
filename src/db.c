#include "cutils/db.h"
#include "cutils/error.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cutils_db {
    sqlite3        *conn;
    pthread_mutex_t mutex;
};

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

    if (pthread_mutex_init(&d->mutex, NULL) != 0) {
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
    pthread_mutex_lock(&db->mutex);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&db->mutex);
        return set_error(CUTILS_ERR_DB, "prepare: %s", sqlite3_errmsg(db->conn));
    }

    rc = bind_params(stmt, params);
    if (rc != CUTILS_OK) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db->mutex);
        return rc;
    }

    int ncols = sqlite3_column_count(stmt);

    /* Collect rows into a dynamic array */
    int capacity = 16;
    int nrows = 0;
    char ***rows = calloc((size_t)capacity, sizeof(char **));
    if (!rows) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db->mutex);
        return set_error(CUTILS_ERR_NOMEM, "db_execute: row array alloc failed");
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (nrows >= capacity) {
            capacity *= 2;
            char ***tmp = realloc(rows, (size_t)capacity * sizeof(char **));
            if (!tmp) {
                /* Free already-collected rows */
                for (int r = 0; r < nrows; r++) {
                    for (int c = 0; c < ncols; c++) free(rows[r][c]);
                    free(rows[r]);
                }
                free(rows);
                sqlite3_finalize(stmt);
                pthread_mutex_unlock(&db->mutex);
                return set_error(CUTILS_ERR_NOMEM, "db_execute: row realloc failed");
            }
            rows = tmp;
        }

        char **row = calloc((size_t)ncols, sizeof(char *));
        if (!row) {
            for (int r = 0; r < nrows; r++) {
                for (int c = 0; c < ncols; c++) free(rows[r][c]);
                free(rows[r]);
            }
            free(rows);
            sqlite3_finalize(stmt);
            pthread_mutex_unlock(&db->mutex);
            return set_error(CUTILS_ERR_NOMEM, "db_execute: column alloc failed");
        }

        for (int c = 0; c < ncols; c++) {
            const char *val = (const char *)sqlite3_column_text(stmt, c);
            row[c] = val ? strdup(val) : strdup("");
        }

        rows[nrows++] = row;
    }

    if (rc != SQLITE_DONE) {
        for (int r = 0; r < nrows; r++) {
            for (int c = 0; c < ncols; c++) free(rows[r][c]);
            free(rows[r]);
        }
        free(rows);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db->mutex);
        return set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
    }

    /* Build column names */
    char **col_names = calloc((size_t)ncols, sizeof(char *));
    if (col_names) {
        for (int c = 0; c < ncols; c++)
            col_names[c] = strdup(sqlite3_column_name(stmt, c));
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db->mutex);

    db_result_t *res = calloc(1, sizeof(*res));
    if (!res) {
        for (int r = 0; r < nrows; r++) {
            for (int c = 0; c < ncols; c++) free(rows[r][c]);
            free(rows[r]);
        }
        free(rows);
        if (col_names) {
            for (int c = 0; c < ncols; c++) free(col_names[c]);
            free(col_names);
        }
        return set_error(CUTILS_ERR_NOMEM, "db_execute: result alloc failed");
    }

    res->rows = rows;
    res->nrows = nrows;
    res->ncols = ncols;
    res->col_names = col_names;
    *result = res;

    return CUTILS_OK;
}

int db_execute_non_query(cutils_db_t *db, const char *sql, const char **params,
                         int *affected)
{
    pthread_mutex_lock(&db->mutex);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&db->mutex);
        return set_error(CUTILS_ERR_DB, "prepare: %s", sqlite3_errmsg(db->conn));
    }

    rc = bind_params(stmt, params);
    if (rc != CUTILS_OK) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db->mutex);
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        int err = set_error(CUTILS_ERR_DB, "step: %s", sqlite3_errmsg(db->conn));
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db->mutex);
        return err;
    }

    if (affected)
        *affected = sqlite3_changes(db->conn);

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db->mutex);

    return CUTILS_OK;
}

int db_exec_raw(cutils_db_t *db, const char *sql)
{
    pthread_mutex_lock(&db->mutex);

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        int err = set_error(CUTILS_ERR_DB, "exec: %s", errmsg ? errmsg : "unknown");
        if (errmsg) sqlite3_free(errmsg);
        pthread_mutex_unlock(&db->mutex);
        return err;
    }

    pthread_mutex_unlock(&db->mutex);
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

/* --- Scoped transaction guard --- */

int cutils_db_tx_begin(cutils_db_t *db, cutils_db_tx_t *tx)
{
    tx->db        = db;
    tx->active    = 0;
    tx->finalized = 0;

    int rc = db_begin(db);
    if (rc == CUTILS_OK)
        tx->active = 1;
    return rc;
}

int db_tx_commit(cutils_db_tx_t *tx)
{
    if (!tx || !tx->active || tx->finalized)
        return CUTILS_OK;

    int rc = db_commit(tx->db);
    if (rc == CUTILS_OK)
        tx->finalized = 1;
    return rc;
}

void cutils_db_tx_end_p(cutils_db_tx_t *tx)
{
    if (!tx) return;
    if (tx->active && !tx->finalized) {
        /* Rollback can fail (e.g. DB closed underneath us); not much we
         * can do in a cleanup handler. Silently attempt and move on. */
        (void)db_rollback(tx->db);
    }
    tx->active    = 0;
    tx->finalized = 1;
}
