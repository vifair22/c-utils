#include "cutils/db.h"
#include "db_internal.h"
#include "cutils_sha256.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* File-local scoped cleanup for DIR handles. */
static void closedir_p(DIR **d)
{
    if (*d) {
        closedir(*d);
        *d = NULL;
    }
}
#define CUTILS_AUTO_DIR __attribute__((cleanup(closedir_p)))

/* --- SHA256 checksum helper --- */

/* Caller guarantees out is at least (CUTILS_SHA256_DIGEST_LEN * 2 + 1)
 * bytes (65). The built-in SHA-256 (1.9.0) produces the same digest as
 * OpenSSL's SHA256(), so checksums recorded by earlier versions match. */
static void sha256_hex(const char *data, size_t len, char *out)
{
    uint8_t hash[CUTILS_SHA256_DIGEST_LEN];
    cutils_sha256(data, len, hash);
    for (int i = 0; i < CUTILS_SHA256_DIGEST_LEN; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[CUTILS_SHA256_DIGEST_LEN * 2] = '\0';
}

/* --- Compiled-in library migrations --- */

static const db_migration_t lib_migrations[] = {
    {
        "001_config_table",
        "CREATE TABLE IF NOT EXISTS config ("
        "    key TEXT PRIMARY KEY,"
        "    value TEXT NOT NULL,"
        "    type TEXT NOT NULL DEFAULT 'string',"
        "    default_value TEXT,"
        "    description TEXT"
        ");"
    },
    {
        "002_logs_table",
        "CREATE TABLE IF NOT EXISTS logs ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    timestamp TEXT NOT NULL,"
        "    level TEXT NOT NULL,"
        "    function TEXT NOT NULL,"
        "    message TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON logs(timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_logs_level ON logs(level);"
    },
    {
        "003_push_table",
        "CREATE TABLE IF NOT EXISTS push ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    timestamp TEXT NOT NULL,"
        "    token TEXT NOT NULL,"
        "    user TEXT NOT NULL,"
        "    ttl TEXT NOT NULL,"
        "    message TEXT NOT NULL,"
        "    title TEXT NOT NULL,"
        "    failed INTEGER NOT NULL DEFAULT 0"
        ");"
    },
    {
        "004_push_html_priority",
        "ALTER TABLE push ADD COLUMN html INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE push ADD COLUMN priority INTEGER NOT NULL DEFAULT 0;"
    },
    {
        "005_push_retry",
        "ALTER TABLE push ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE push ADD COLUMN next_retry_at INTEGER NOT NULL DEFAULT 0;"
    },
    { NULL, NULL }
};

/* --- Migration tracking table --- */

static const char *TRACKING_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS system_migrations ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    filename TEXT NOT NULL UNIQUE,"
    "    checksum TEXT NOT NULL,"
    "    applied_at TEXT NOT NULL DEFAULT (datetime('now'))"
    ");";

/* Fail a migration step with context (1.8.2, c-backup B288). Captures
 * SQLite's extended code and the message of the call that just failed,
 * before anything else runs on the handle, and reports
 * "<context> (sqlite error <code>): <message>". Before this, steps
 * outside a single migration's SQL (tracking table, BEGIN, checksum
 * lookup, recording, COMMIT) returned the bare db error, so a COMMIT
 * failing with SQLITE_FULL read "exec: database or disk is full" with no
 * hint of where. */
__attribute__((format(printf, 2, 3)))
static int migrate_fail(cutils_db_t *db, const char *fmt, ...)
{
    int  code = db_last_errcode(db);
    char cause[CUTILS_ERR_BUFSZ];
    snprintf(cause, sizeof(cause), "%s", cutils_get_error());

    char ctx[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx, sizeof(ctx), fmt, ap);
    va_end(ap);

    return set_error(CUTILS_ERR_MIGRATE, "%s (sqlite error %d): %s",
                     ctx, code, cause);
}

/* Report a failed batch COMMIT, naming the last migration applied in the
 * batch: on SQLITE_FULL or a deferred constraint, the COMMIT is where
 * that migration's writes actually fail. */
static int commit_fail(cutils_db_t *db, const char *last_applied)
{
    if (last_applied[0])
        return migrate_fail(db, "migrations commit failed, last applied %s",
                            last_applied);
    return migrate_fail(db, "migrations commit failed (none applied in this batch)");
}

/* Check if a migration has been applied and verify checksum */
static int check_migration(cutils_db_t *db, const char *tracked_name,
                           const char *checksum, int *applied)
{
    *applied = 0;
    const char *params[] = { tracked_name, NULL };
    CUTILS_AUTO_DBRES db_result_t *result = NULL;

    int rc = db_execute(db,
        "SELECT checksum FROM system_migrations WHERE filename = ?",
        params, &result);
    if (rc != CUTILS_OK)
        return migrate_fail(db, "migration %s: checksum lookup failed",
                            tracked_name);

    if (result->nrows > 0) {
        const char *stored = result->rows[0][0];
        if (strcmp(stored, checksum) != 0)
            return set_error(CUTILS_ERR_MIGRATE,
                "checksum mismatch for %s: expected %s, got %s",
                tracked_name, stored, checksum);
        *applied = 1;
    }

    return CUTILS_OK;
}

/* Record a migration as applied */
static int record_migration(cutils_db_t *db, const char *tracked_name,
                            const char *checksum)
{
    const char *params[] = { tracked_name, checksum, NULL };
    return db_execute_non_query(db,
        "INSERT INTO system_migrations (filename, checksum) VALUES (?, ?)",
        params, NULL);
}

/* Undo a failed migration's savepoint. SQLite may already have rolled
 * back the whole transaction when the statement failed (temp-store
 * failure, SQLITE_FULL, SQLITE_IOERR, RAISE(ROLLBACK)); the savepoint
 * went with it, and ROLLBACK TO would only fail with "no such
 * savepoint". The caller restores its own error afterwards, so a
 * failure here has nothing to add. */
static void rollback_savepoint(cutils_db_t *db, const char *sp_name)
{
    if (db_in_transaction(db))
        CUTILS_UNUSED(db_savepoint_rollback(db, sp_name));
}

/* Apply a single migration within a savepoint */
static int apply_migration(cutils_db_t *db, const char *tracked_name,
                           const char *sql, const char *checksum)
{
    /* Build savepoint name from tracked_name (strip prefix, replace dots) */
    char sp_name[128];
    snprintf(sp_name, sizeof(sp_name), "mig_%s", tracked_name);
    for (char *p = sp_name; *p; p++) {
        if (*p == '/' || *p == '.' || *p == '-') *p = '_';
    }

    int rc = db_savepoint(db, sp_name);
    if (rc != CUTILS_OK) return rc;

    rc = db_exec_raw(db, sql);
    if (rc != CUTILS_OK) {
        /* Capture the statement's own error before any recovery runs:
         * the code first, since any later call on the handle replaces
         * it, then the message, which a failing rollback would replace
         * (1.8.1, c-backup B244). */
        int  code = db_last_errcode(db);
        char cause[CUTILS_ERR_BUFSZ];
        snprintf(cause, sizeof(cause), "%s", cutils_get_error());
        rollback_savepoint(db, sp_name);
        return set_error(CUTILS_ERR_MIGRATE,
                         "migration %s failed (sqlite error %d): %s",
                         tracked_name, code, cause);
    }

    rc = record_migration(db, tracked_name, checksum);
    if (rc != CUTILS_OK) {
        rc = migrate_fail(db, "migration %s: recording failed", tracked_name);
        char cause[CUTILS_ERR_BUFSZ];
        snprintf(cause, sizeof(cause), "%s", cutils_get_error());
        rollback_savepoint(db, sp_name);
        return set_error(rc, "%s", cause);
    }

    return db_savepoint_release(db, sp_name);
}

/* --- Shared: run a db_migration_t array with optional name prefix --- */

static int run_migration_array(cutils_db_t *db, const db_migration_t *migrations,
                               const char *prefix)
{
    /* Ensure tracking table exists */
    int rc = db_exec_raw(db, TRACKING_TABLE_SQL);
    if (rc != CUTILS_OK)
        return migrate_fail(db, "migrations: tracking table setup failed");

    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
    rc = cutils_db_tx_begin(db, &tx);
    if (rc != CUTILS_OK)
        return migrate_fail(db, "migrations: begin failed");

    char last_applied[256] = "";

    for (int i = 0; migrations[i].name != NULL; i++) {
        const char *name = migrations[i].name;
        const char *sql = migrations[i].sql;

        /* Build tracked name (with prefix if provided) */
        char tracked[256];
        if (prefix)
            snprintf(tracked, sizeof(tracked), "%s%s", prefix, name);
        else
            snprintf(tracked, sizeof(tracked), "%s", name);

        /* Compute checksum */
        char checksum[65];
        sha256_hex(sql, strlen(sql), checksum);

        int applied = 0;
        rc = check_migration(db, tracked, checksum, &applied);
        if (rc != CUTILS_OK) return rc;   /* tx auto-rolls-back */

        if (applied) continue;

        fprintf(stderr, "Migration: applying %s\n", tracked);
        rc = apply_migration(db, tracked, sql, checksum);
        if (rc != CUTILS_OK) return rc;   /* tx auto-rolls-back */
        snprintf(last_applied, sizeof(last_applied), "%s", tracked);
    }

    if (db_tx_commit(&tx) != CUTILS_OK)
        return commit_fail(db, last_applied); /* tx auto-rolls-back */
    return CUTILS_OK;
}

/* --- Public API: run compiled-in library migrations --- */

int db_run_lib_migrations(cutils_db_t *db)
{
    return run_migration_array(db, lib_migrations, "_lib/");
}

/* --- Public API: run compiled-in app migrations --- */

int db_run_compiled_migrations(cutils_db_t *db, const db_migration_t *migrations)
{
    if (!migrations) return CUTILS_OK;
    return run_migration_array(db, migrations, NULL);
}

/* --- Public API: run file-based app migrations --- */

/* Compare function for qsort on dirent name strings */
static int cmp_strings(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

int db_run_app_migrations(cutils_db_t *db, const char *migrations_dir)
{
    if (!migrations_dir) return CUTILS_OK;

    CUTILS_AUTO_DIR DIR *dir = opendir(migrations_dir);
    if (!dir) return CUTILS_OK; /* No directory = no migrations */

    /* Ensure tracking table exists */
    int rc = db_exec_raw(db, TRACKING_TABLE_SQL);
    if (rc != CUTILS_OK)
        return migrate_fail(db, "migrations: tracking table setup failed");

    /* tx is declared before any goto to cleanup: — C forbids goto-ing past
     * a cleanup-attributed declaration. Zero-initialized so the cleanup
     * is a no-op until cutils_db_tx_begin() activates it. */
    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };

    /* Collect .sql filenames. files is freed at the cleanup label. */
    int capacity = 16;
    int count    = 0;
    char **files = calloc((size_t)capacity, sizeof(char *));
    if (!files)
        return set_error(CUTILS_ERR_NOMEM, "migration file list alloc failed");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, ".sql") != 0)
            continue;

        if (count >= capacity) {
            capacity *= 2;
            char **tmp = realloc(files, (size_t)capacity * sizeof(char *));
            if (!tmp) {
                rc = set_error(CUTILS_ERR_NOMEM, "migration file list realloc");
                goto cleanup;
            }
            files = tmp;
        }
        files[count++] = strdup(entry->d_name);
    }

    if (count == 0) {
        rc = CUTILS_OK;
        goto cleanup;
    }

    /* Sort lexically */
    qsort(files, (size_t)count, sizeof(char *), cmp_strings);

    /* Apply migrations in a single transaction */
    char last_applied[256] = "";
    rc = cutils_db_tx_begin(db, &tx);
    if (rc != CUTILS_OK) {
        rc = migrate_fail(db, "migrations: begin failed");
        goto cleanup;
    }

    for (int i = 0; i < count; i++) {
        /* Read file contents */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", migrations_dir, files[i]);

        CUTILS_AUTOCLOSE FILE *f = fopen(path, "r");
        if (!f) {
            rc = set_error_errno(CUTILS_ERR_IO, "open migration %s", path);
            goto cleanup;          /* tx auto-rolls-back */
        }

        /* LCOV_EXCL_START — fseek / ftell only fail on stream-type
         * mismatches (pipes, char devices, etc.) and EBADF; for a
         * just-opened regular .sql file, these are unreachable in
         * any practical test. */
        if (fseek(f, 0, SEEK_END) != 0) {
            rc = set_error_errno(CUTILS_ERR_IO, "fseek %s", path);
            goto cleanup;
        }
        long fsize = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0) {
            rc = set_error_errno(CUTILS_ERR_IO, "fseek %s", path);
            goto cleanup;
        }

        if (fsize < 0) {
            /* ftell error — surface it instead of silently skipping. */
            rc = set_error_errno(CUTILS_ERR_IO, "ftell %s", path);
            goto cleanup;
        }
        /* LCOV_EXCL_STOP */
        if (fsize == 0) continue;  /* empty .sql file, skip silently */

        CUTILS_AUTOFREE char *sql = malloc((size_t)fsize + 1);
        if (!sql) {
            rc = set_error(CUTILS_ERR_NOMEM, "migration %s: alloc failed",
                           files[i]);
            goto cleanup;          /* tx auto-rolls-back */
        }

        size_t nread = fread(sql, 1, (size_t)fsize, f);
        sql[nread] = '\0';

        /* Compute checksum */
        char checksum[65];
        sha256_hex(sql, nread, checksum);

        int applied = 0;
        rc = check_migration(db, files[i], checksum, &applied);
        if (rc != CUTILS_OK) goto cleanup;   /* tx auto-rolls-back */

        if (applied) continue;

        fprintf(stderr, "Migration: applying %s\n", files[i]);
        rc = apply_migration(db, files[i], sql, checksum);
        if (rc != CUTILS_OK) goto cleanup;   /* tx auto-rolls-back */
        snprintf(last_applied, sizeof(last_applied), "%s", files[i]);
    }

    rc = db_tx_commit(&tx);
    if (rc != CUTILS_OK)
        rc = commit_fail(db, last_applied);  /* tx auto-rolls-back */

cleanup:
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    return rc;
}
