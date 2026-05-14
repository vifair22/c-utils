/* examples/minimal — a buildable end-to-end c-utils consumer.
 *
 * Goes from main() to a fully-initialized app (config parsed, DB
 * opened, migrations run, log writer + push worker + signal watcher
 * spawned) with one appguard_init call. Demonstrates:
 *
 *   - Defining app-specific config keys (one file-backed, one DB-backed)
 *   - Reading config values
 *   - Logging at each level
 *   - Querying the DB via the streaming iterator
 *   - Sending a notification (no-ops cleanly if pushover isn't configured)
 *   - Graceful shutdown
 *
 * On first run with no config.yaml present, AppGuard generates a
 * template config (mode 0600) and prints the path. Run again to use it.
 *
 * Build: `make` in this directory (sees c-utils via ../../).
 * Run:   `./minimal`
 *
 * To exercise the streaming iterator, the example seeds a small table
 * during init.
 */

#include <cutils.h>

#include <stdio.h>
#include <string.h>

/* App-specific config keys. The CFG_STORE_FILE keys are emitted into
 * the generated YAML template on first run. */
static const config_key_t app_file_keys[] = {
    { "app.name",     CFG_STRING, "minimal-example",
      "Display name shown in log lines", CFG_STORE_FILE, 1 },
    { "app.greeting", CFG_STRING, "hello, world",
      "First line the example logs at startup", CFG_STORE_FILE, 0 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

/* A single DB-backed key, just to demonstrate the two-store split.
 * Seeded by appguard via config_attach_db on first run. */
static const config_key_t app_db_keys[] = {
    { "runtime.greeted_count", CFG_INT, "0",
      "How many times this example has been greeted", CFG_STORE_DB, 0 },
    { NULL, 0, NULL, NULL, 0, 0 }
};

static const config_section_t app_sections[] = {
    { "app",     "Application Settings" },
    { "runtime", "Runtime State" },
    { NULL, NULL }
};

/* Compiled-in app migration: a tiny demo table. */
static const db_migration_t app_migrations[] = {
    { "001_widgets",
      "CREATE TABLE IF NOT EXISTS widgets ("
      "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "    name TEXT NOT NULL"
      ");" },
    { NULL, NULL }
};

static int seed_widgets(cutils_db_t *db)
{
    static const char *names[] = { "alpha", "beta", "gamma", NULL };
    for (int i = 0; names[i]; i++) {
        const char *p[] = { names[i], NULL };
        int rc = db_execute_non_query(db,
            "INSERT INTO widgets (name) VALUES (?)", p, NULL);
        if (rc != CUTILS_OK) {
            log_error("seed_widgets: %s", cutils_get_error());
            return rc;
        }
    }
    return CUTILS_OK;
}

static int demo_iterate(cutils_db_t *db)
{
    /* Stream every row in the widgets table; no upper bound on size,
     * O(1) heap regardless. */
    CUTILS_AUTO_DB_ITER cutils_db_iter_t *it = NULL;
    int rc = db_iter_begin(db,
        "SELECT id, name FROM widgets ORDER BY id", NULL, &it);
    if (rc != CUTILS_OK) {
        log_error("widgets iter: %s", cutils_get_error());
        return rc;
    }

    const char **row;
    while (db_iter_next(it, &row))
        log_info("widget: id=%s name=%s", row[0], row[1]);

    return db_iter_error(it) ? CUTILS_ERR_DB : CUTILS_OK;
}

int main(int argc, char **argv)
{
    appguard_set_argv(argc, argv);

    appguard_config_t cfg = {
        .app_name               = "minimal-example",
        .on_first_run           = CFG_FIRST_RUN_EXIT,
        .file_keys              = app_file_keys,
        .db_keys                = app_db_keys,
        .sections               = app_sections,
        .migrations             = app_migrations,
        .log_level              = LOG_INFO,
        .log_retention_days     = 7,
        .log_systemd_autodetect = 1,
        .enable_pushover        = 1, /* silently disabled if creds absent */
    };

    appguard_t *guard = appguard_init(&cfg);
    if (!guard) {
        /* First-run-exit path returns NULL after generating the
         * template; the message has already been printed to stderr. */
        return 0;
    }

    cutils_config_t *config = appguard_config(guard);
    cutils_db_t     *db     = appguard_db(guard);

    log_info("started: %s", config_get_str(config, "app.name"));
    log_info("greeting: %s", config_get_str(config, "app.greeting"));

    /* Seed widgets on first run only — checked by counting rows. */
    db_result_t *r = NULL;
    if (db_execute(db, "SELECT COUNT(*) FROM widgets", NULL, &r)
        == CUTILS_OK && r->nrows == 1 && r->rows[0][0]
        && strcmp(r->rows[0][0], "0") == 0) {
        log_info("seeding widgets table");
        seed_widgets(db);
    }
    db_result_free(r);

    demo_iterate(db);

    /* Bump the DB-backed counter so subsequent runs differ. */
    int n = config_get_int(config, "runtime.greeted_count", 0) + 1;
    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "%d", n);
    if (config_set_db(config, "runtime.greeted_count", nbuf) == CUTILS_OK)
        log_info("greeted %d time(s) total", n);

    /* push_send is a silent no-op when pushover creds aren't set. */
    if (push_send("minimal-example", "started up successfully") != CUTILS_OK)
        log_warn("push enqueue failed: %s", cutils_get_error());

    appguard_shutdown(guard);
    return 0;
}
