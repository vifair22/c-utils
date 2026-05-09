#include "cutils/push.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"
#include "cutils/mem.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* File-local scoped cleanup for CURL resources. */
static void curl_easy_cleanup_p(CURL **c)
{
    if (*c) {
        curl_easy_cleanup(*c);
        *c = NULL;
    }
}
#define CUTILS_AUTO_CURL __attribute__((cleanup(curl_easy_cleanup_p)))

static void curl_free_p(char **s)
{
    if (*s) {
        curl_free(*s);
        *s = NULL;
    }
}
#define CUTILS_AUTO_CURLSTR __attribute__((cleanup(curl_free_p)))

#define PUSHOVER_URL     "https://api.pushover.net/1/messages.json"
#define MAX_MSG_CHARS    1024
#define MAX_TITLE_CHARS  250
#define DEFAULT_TTL      86400

/* Backoff schedule for transient send failures. Index = number of
 * prior failures on the row; value = seconds to wait before the
 * next attempt. The final entry caps the delay; any further failure
 * reuses it until the message's TTL expires (then it's marked
 * failed=1 and stops retrying). The 4-second total cap of the
 * pre-1.0.2 inline retry was well shorter than a router restart,
 * so transient outages would silently swallow messages. This
 * schedule stretches across realistic outage durations (router
 * reboots, ISP blips, brief cloud incidents) and gives up only
 * when the per-message TTL says to. */
static const int push_backoff_schedule[] = {
    30, 60, 120, 300, 900, 3600
};
static const size_t push_backoff_schedule_len =
    sizeof(push_backoff_schedule) / sizeof(push_backoff_schedule[0]);

static int push_backoff_seconds(int attempts)
{
    if (attempts < 1) attempts = 1;
    size_t idx = (size_t)(attempts - 1);
    if (idx >= push_backoff_schedule_len)
        idx = push_backoff_schedule_len - 1;
    return push_backoff_schedule[idx];
}

/* --- Module state ---
 *
 * push_db / push_cfg are read by push_send (any caller thread) and
 * written by push_init / push_shutdown — atomic load/store gives
 * defined semantics under concurrent shutdown. push_running is the
 * outer "is the worker alive" gate for push_shutdown; also atomic.
 *
 * push_stop and push_notify are accessed only under push_mutex (the
 * cond-var pair), so plain int is correct. */

static _Atomic(cutils_db_t *)            push_db      = NULL;
static _Atomic(const cutils_config_t *)  push_cfg     = NULL;
static pthread_t                         push_thread;
static pthread_mutex_t                   push_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t                    push_cond    = PTHREAD_COND_INITIALIZER;
static int                               push_stop    = 0;  /* push_mutex */
static _Atomic int                       push_running = 0;
static int                               push_notify  = 0;  /* push_mutex */

/* --- curl write callback (discard response body) --- */

static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
}

/* --- Message splitting ---
 *
 * All chunks of a multi-part message are inserted under a single
 * transaction so concurrent push_send calls can't interleave each
 * other's chunks (Pushover would deliver "A 1/2", "B 1/2", "A 2/2",
 * "B 2/2" otherwise). The worker drains by rowid (insertion order),
 * not by timestamp, so the per-part timestamp offset that the
 * pre-fix code added is now unnecessary — every chunk uses the same
 * caller-provided timestamp. */

static int split_and_store(const char *title, const char *message,
                           const char *token, const char *user,
                           int ttl, int timestamp, int html, int priority)
{
    cutils_db_t *db = atomic_load(&push_db);
    if (!db) return set_error(CUTILS_ERR_INVALID, "push not initialized");

    size_t msglen = strlen(message);
    char ts_str[32], ttl_str[16], html_str[4], pri_str[4];
    snprintf(ts_str, sizeof(ts_str), "%d", timestamp);
    snprintf(ttl_str, sizeof(ttl_str), "%d", ttl);
    snprintf(html_str, sizeof(html_str), "%d", html);
    snprintf(pri_str, sizeof(pri_str), "%d", priority);

    if (msglen <= MAX_MSG_CHARS) {
        const char *params[] = {
            ts_str, token, user, ttl_str, message, title,
            html_str, pri_str, NULL
        };
        return db_execute_non_query(db,
            "INSERT INTO push (timestamp, token, user, ttl, message, title, "
            "failed, html, priority) VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?)",
            params, NULL);
    }

    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
    int rc = cutils_db_tx_begin(db, &tx);
    if (rc != CUTILS_OK) return rc;

    /* Split on newline boundaries */
    const char *remaining = message;
    size_t rem_len = msglen;
    int part = 0;
    int total = (int)((msglen + MAX_MSG_CHARS - 1) / MAX_MSG_CHARS); /* estimate */

    while (rem_len > 0) {
        size_t chunk_len;
        if (rem_len <= MAX_MSG_CHARS) {
            chunk_len = rem_len;
        } else {
            /* Find last newline within limit */
            chunk_len = MAX_MSG_CHARS;
            const char *nl = remaining + MAX_MSG_CHARS;
            while (nl > remaining && *nl != '\n') nl--;
            if (nl > remaining)
                chunk_len = (size_t)(nl - remaining) + 1;
        }

        /* Build part title */
        char part_title[MAX_TITLE_CHARS + 1];
        char suffix[32];
        snprintf(suffix, sizeof(suffix), " (%d/%d)", part + 1, total);
        size_t max_base = (size_t)MAX_TITLE_CHARS - strlen(suffix);
        size_t title_len = strlen(title);
        if (title_len > max_base) title_len = max_base;
        snprintf(part_title, sizeof(part_title), "%.*s%s",
                 (int)title_len, title, suffix);

        /* Build chunk string */
        CUTILS_AUTOFREE char *chunk = malloc(chunk_len + 1);
        if (!chunk) return set_error(CUTILS_ERR_NOMEM, "push split alloc");
        memcpy(chunk, remaining, chunk_len);
        chunk[chunk_len] = '\0';

        const char *params[] = {
            ts_str, token, user, ttl_str, chunk, part_title,
            html_str, pri_str, NULL
        };
        rc = db_execute_non_query(db,
            "INSERT INTO push (timestamp, token, user, ttl, message, title, "
            "failed, html, priority) VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?)",
            params, NULL);

        if (rc != CUTILS_OK) return rc;

        remaining += chunk_len;
        rem_len -= chunk_len;
        part++;
    }

    return db_tx_commit(&tx);
}

/* --- Single-attempt send ---
 *
 * The pre-1.0.2 version retried inline with sleep() (1s, 2s, 4s) inside
 * this function. That had two serious problems: (1) the 7s total budget
 * was shorter than a router restart, so a brief outage would silently
 * drop the message; (2) sleep() blocked push_shutdown for up to 4s.
 * Retry is now the worker's job — it tracks attempts/next_retry_at in
 * the DB and uses pthread_cond_timedwait so shutdown stays responsive
 * across arbitrarily long backoff windows. */

typedef enum {
    SEND_OK,
    SEND_PERMANENT_FAIL,
    SEND_TRANSIENT_FAIL,
} send_result_t;

static send_result_t send_one(const char *token, const char *user,
                              const char *title, const char *message,
                              const char *timestamp, const char *ttl,
                              int html, int priority)
{
    CUTILS_AUTO_CURL CURL *curl = curl_easy_init();
    if (!curl) return SEND_TRANSIENT_FAIL;

    curl_easy_setopt(curl, CURLOPT_URL, PUSHOVER_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    char postfields[4096];
    CUTILS_AUTO_CURLSTR char *enc_title = curl_easy_escape(curl, title, 0);
    CUTILS_AUTO_CURLSTR char *enc_msg   = curl_easy_escape(curl, message, 0);

    int written = snprintf(postfields, sizeof(postfields),
             "token=%s&user=%s&title=%s&message=%s&timestamp=%s&ttl=%s",
             token, user,
             enc_title ? enc_title : "",
             enc_msg ? enc_msg : "",
             timestamp, ttl);

    if (html)
        written += snprintf(postfields + written,
                            sizeof(postfields) - (size_t)written,
                            "&html=1");
    if (priority != 0)
        written += snprintf(postfields + written,
                            sizeof(postfields) - (size_t)written,
                            "&priority=%d", priority);
    (void)written;

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300)
        return SEND_OK;

    if (res != CURLE_OK)
        return SEND_TRANSIENT_FAIL; /* Network/DNS/timeout — retryable. */

    if (http_code >= 400 && http_code < 500)
        return SEND_PERMANENT_FAIL; /* Bad token, malformed request, etc. */

    return SEND_TRANSIENT_FAIL;     /* 5xx — retryable. */
}

/* --- Worker thread ---
 *
 * Wait/drain cycle:
 *
 * 1. Compute the earliest next_retry_at across non-failed rows. If
 *    one exists in the future, the wait below uses it as the deadline.
 * 2. cond_wait (forever) or cond_timedwait (until deadline). Either
 *    can be cut short by push_send (push_notify=1) or push_shutdown
 *    (push_stop=1 + push_notify=1) signaling the cond. This is what
 *    makes shutdown responsive even when the next retry is hours
 *    away — we don't sleep().
 * 3. Drain every row that's currently due (failed=0 AND
 *    next_retry_at <= now), in rowid order. For each:
 *      - TTL expired (now > timestamp + ttl): mark failed=1, skip.
 *      - send OK: DELETE.
 *      - permanent (4xx): mark failed=1.
 *      - transient: attempts++, next_retry_at = now + backoff(attempts).
 *    Transient-failed rows drop out of the SELECT predicate and the
 *    drain advances to the next due row instead of breaking — a single
 *    flaky row can't park siblings behind it. */

static time_t now_secs(void)
{
    return time(NULL);
}

static time_t earliest_next_retry(cutils_db_t *db)
{
    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(db,
        "SELECT next_retry_at FROM push WHERE failed = 0 "
        "ORDER BY next_retry_at ASC LIMIT 1",
        NULL, &result);
    if (rc != CUTILS_OK || !result || result->nrows == 0 || !result->rows[0][0])
        return 0;
    long v = strtol(result->rows[0][0], NULL, 10);
    return v > 0 ? (time_t)v : 0;
}

static void *push_worker_thread(void *arg)
{
    (void)arg;

    while (1) {
        cutils_db_t *db = atomic_load(&push_db);
        if (!db) break; /* Shutdown nulled push_db. */

        time_t wakeup_at = earliest_next_retry(db);

        int stopping;
        {
            CUTILS_LOCK_GUARD(&push_mutex);
            while (!push_notify && !push_stop) {
                if (wakeup_at > 0) {
                    time_t now = now_secs();
                    if (now >= wakeup_at) break; /* Already due. */
                    struct timespec ts = { wakeup_at, 0 };
                    int wrc = pthread_cond_timedwait(&push_cond,
                                                    &push_mutex, &ts);
                    if (wrc == ETIMEDOUT) break; /* Time to drain. */
                } else {
                    pthread_cond_wait(&push_cond, &push_mutex);
                }
            }
            push_notify = 0;
            stopping    = push_stop;
        }

        /* Drain even when stopping — appguard.h header advertises
         * "drains pending messages first" on shutdown. Rows still in
         * retry-backoff (next_retry_at > now) are skipped by the
         * SELECT predicate and resurface on the next process start. */
        while (1) {
            CUTILS_AUTO_DBRES db_result_t *result = NULL;
            time_t now = now_secs();
            char now_str[32];
            snprintf(now_str, sizeof(now_str), "%lld", (long long)now);
            const char *sel_params[] = { now_str, NULL };

            /* ORDER BY rowid: strict insertion order. Multi-part
             * messages from a single push_send share next_retry_at=0
             * (insert default) and one tx, so concurrent senders
             * never interleave parts and a sender's parts always
             * deliver in order. Rows in retry-backoff have
             * next_retry_at > now and are skipped here, so a single
             * flaky row never blocks queued siblings. */
            int rc = db_execute(push_db,
                "SELECT rowid, timestamp, token, user, ttl, message, title, "
                "html, priority, attempts "
                "FROM push WHERE failed = 0 AND next_retry_at <= ? "
                "ORDER BY rowid LIMIT 1",
                sel_params, &result);

            if (rc != CUTILS_OK || !result || result->nrows == 0)
                break;

            const char *rowid     = result->rows[0][0];
            const char *timestamp = result->rows[0][1];
            const char *token     = result->rows[0][2];
            const char *user      = result->rows[0][3];
            const char *ttl       = result->rows[0][4];
            const char *message   = result->rows[0][5];
            const char *title     = result->rows[0][6];
            int html              = atoi(result->rows[0][7]);
            int priority          = atoi(result->rows[0][8]);
            int attempts          = atoi(result->rows[0][9]);

            /* TTL give-up: if the message has been queued past its
             * declared lifetime, drop it without sending. The default
             * TTL is 24h (DEFAULT_TTL); apps can override per-message
             * via push_opts_t.ttl. Pushover-side rendering of stale
             * alerts is worse than not delivering them. */
            long long row_ts  = strtoll(timestamp, NULL, 10);
            long long row_ttl = strtoll(ttl, NULL, 10);
            if (row_ts > 0 && row_ttl > 0 && (long long)now > row_ts + row_ttl) {
                const char *fail_params[] = { rowid, NULL };
                CUTILS_UNUSED(db_execute_non_query(push_db,
                    "UPDATE push SET failed = 1 WHERE rowid = ?",
                    fail_params, NULL));
                log_warn("push expired before delivery (ttl reached): %s", title);
                continue;
            }

            send_result_t sr = send_one(token, user, title, message,
                                        timestamp, ttl, html, priority);

            if (sr == SEND_OK) {
                const char *del_params[] = { rowid, NULL };
                /* Best-effort: if the delete fails, the row will be re-delivered
                 * next cycle. Not catastrophic — Pushover collapses dupes. */
                CUTILS_UNUSED(db_execute_non_query(push_db,
                    "DELETE FROM push WHERE rowid = ?", del_params, NULL));
                log_debug("push sent: %s", title);
            } else if (sr == SEND_PERMANENT_FAIL) {
                const char *fail_params[] = { rowid, NULL };
                /* Best-effort: if marking fails, the row retries next cycle. */
                CUTILS_UNUSED(db_execute_non_query(push_db,
                    "UPDATE push SET failed = 1 WHERE rowid = ?",
                    fail_params, NULL));
                log_error("push permanently failed: %s", title);
            } else {
                int next_attempts = attempts + 1;
                int delay         = push_backoff_seconds(next_attempts);
                long long retry_at = (long long)now + delay;
                char attempts_str[16], retry_at_str[32];
                snprintf(attempts_str, sizeof(attempts_str), "%d", next_attempts);
                snprintf(retry_at_str, sizeof(retry_at_str), "%lld", retry_at);
                const char *upd_params[] = {
                    attempts_str, retry_at_str, rowid, NULL
                };
                CUTILS_UNUSED(db_execute_non_query(push_db,
                    "UPDATE push SET attempts = ?, next_retry_at = ? "
                    "WHERE rowid = ?", upd_params, NULL));
                log_warn("push transient failure (attempt %d, retry in %ds): %s",
                         next_attempts, delay, title);
                /* Don't break — the retried row drops out of the
                 * SELECT predicate and the drain advances to the
                 * next due row. */
            }
        }

        if (stopping) break;
    }

    return NULL;
}

/* --- Public API --- */

int push_init(cutils_db_t *db, const cutils_config_t *cfg)
{
    atomic_store(&push_db, db);
    atomic_store(&push_cfg, cfg);
    push_stop = 0;
    /* Kick the worker into draining on its first iteration. The push
     * table can hold rows from a previous (possibly crashed) run that
     * never got delivered — the header explicitly promises "messages
     * survive crashes — the worker picks up unsent messages on
     * restart". With push_notify left at 0, the worker would park on
     * cond_wait at startup and only drain once a fresh push_send came
     * in, contradicting that contract. */
    push_notify = 1;
    atomic_store(&push_running, 1);

    /* Verify creds are configured. When missing, null out push_db so
     * push_send becomes a silent no-op — same contract as appguard
     * skipping push_init entirely (enable_pushover=0). Pre-1.0.2 left
     * push_db set, which meant push_send happily inserted rows that
     * nothing drained: a slow queue leak. */
    const char *token = config_get_str(cfg, PUSH_CONFIG_TOKEN);
    const char *user  = config_get_str(cfg, PUSH_CONFIG_USER);
    if (!token || !token[0] || !user || !user[0]) {
        log_info("Pushover not configured, notifications disabled");
        atomic_store(&push_db, (cutils_db_t *)NULL);
        atomic_store(&push_cfg, (const cutils_config_t *)NULL);
        atomic_store(&push_running, 0);
        return CUTILS_OK;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int rc = pthread_create(&push_thread, NULL, push_worker_thread, NULL);
    if (rc != 0) {
        atomic_store(&push_running, 0);
        return set_error(CUTILS_ERR, "failed to create push worker thread");
    }

    log_info("Pushover notifications enabled");
    return CUTILS_OK;
}

void push_shutdown(void)
{
    if (!atomic_load(&push_running)) return;
    atomic_store(&push_running, 0);

    {
        CUTILS_LOCK_GUARD(&push_mutex);
        push_stop = 1;
        push_notify = 1;
        pthread_cond_signal(&push_cond);
    }

    pthread_join(push_thread, NULL);
    curl_global_cleanup();

    /* Clear handles AFTER the worker has been joined. push_send sees
     * push_db == NULL and returns CUTILS_OK as a silent no-op — same
     * disabled contract as appguard skipping push_init. Late callers
     * during shutdown drop their messages instead of inserting into a
     * queue with no live worker to drain them. */
    atomic_store(&push_db, (cutils_db_t *)NULL);
    atomic_store(&push_cfg, (const cutils_config_t *)NULL);
}

int push_send(const char *title, const char *message)
{
    push_opts_t opts = {
        .title = title,
        .message = message,
    };
    return push_send_opts(&opts);
}

int push_send_opts(const push_opts_t *opts)
{
    /* Disabled contract: push_db NULL means either appguard set
     * enable_pushover=0, or creds are missing, or push_shutdown ran.
     * In all three cases the message is silently dropped — the app
     * code sprinkles push_send() calls without conditional gates. */
    if (!atomic_load(&push_db))
        return CUTILS_OK;

    const char *token = opts->token;
    const char *user  = opts->user;
    int ttl           = opts->ttl > 0 ? opts->ttl : DEFAULT_TTL;
    int timestamp     = opts->timestamp > 0 ? opts->timestamp : (int)time(NULL);

    const cutils_config_t *cfg = atomic_load(&push_cfg);
    if (!token || !token[0])
        token = config_get_str(cfg, PUSH_CONFIG_TOKEN);
    if (!user || !user[0])
        user = config_get_str(cfg, PUSH_CONFIG_USER);

    if (!token || !token[0] || !user || !user[0])
        return set_error(CUTILS_ERR_CONFIG, "Pushover credentials not configured");

    int rc = split_and_store(opts->title, opts->message,
                             token, user, ttl, timestamp,
                             opts->html, opts->priority);
    if (rc != CUTILS_OK) return rc;

    /* Wake the worker */
    {
        CUTILS_LOCK_GUARD(&push_mutex);
        push_notify = 1;
        pthread_cond_signal(&push_cond);
    }

    return CUTILS_OK;
}
