#include "cutils/push.h"
#include "cutils/config.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/log.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PUSHOVER_URL     "https://api.pushover.net/1/messages.json"
#define MAX_MSG_CHARS    1024
#define MAX_TITLE_CHARS  250
#define MAX_RETRIES      3
#define BASE_DELAY_SEC   1
#define DEFAULT_TTL      86400

/* --- Module state --- */

static cutils_db_t           *push_db     = NULL;
static const cutils_config_t *push_cfg    = NULL;
static pthread_t              push_thread;
static pthread_mutex_t        push_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t         push_cond   = PTHREAD_COND_INITIALIZER;
static int                    push_stop   = 0;
static int                    push_running = 0;
static int                    push_notify = 0;  /* new message signal */

/* --- curl write callback (discard response body) --- */

static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
}

/* --- Message splitting --- */

static int split_and_store(const char *title, const char *message,
                           const char *token, const char *user,
                           int ttl, int timestamp, int html, int priority)
{
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
        return db_execute_non_query(push_db,
            "INSERT INTO push (timestamp, token, user, ttl, message, title, "
            "failed, html, priority) VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?)",
            params, NULL);
    }

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
        char *chunk = malloc(chunk_len + 1);
        if (!chunk) return set_error(CUTILS_ERR_NOMEM, "push split alloc");
        memcpy(chunk, remaining, chunk_len);
        chunk[chunk_len] = '\0';

        char part_ts[32];
        snprintf(part_ts, sizeof(part_ts), "%d", timestamp + part);

        const char *params[] = {
            part_ts, token, user, ttl_str, chunk, part_title,
            html_str, pri_str, NULL
        };
        int rc = db_execute_non_query(push_db,
            "INSERT INTO push (timestamp, token, user, ttl, message, title, "
            "failed, html, priority) VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?)",
            params, NULL);

        free(chunk);
        if (rc != CUTILS_OK) return rc;

        remaining += chunk_len;
        rem_len -= chunk_len;
        part++;
    }

    return CUTILS_OK;
}

/* --- Send with retry --- */

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
    int attempt = 0;
    int delay = BASE_DELAY_SEC;

    while (attempt < MAX_RETRIES) {
        CURL *curl = curl_easy_init();
        if (!curl) return SEND_TRANSIENT_FAIL;

        curl_easy_setopt(curl, CURLOPT_URL, PUSHOVER_URL);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        char postfields[4096];
        char *enc_title = curl_easy_escape(curl, title, 0);
        char *enc_msg   = curl_easy_escape(curl, message, 0);

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

        curl_free(enc_title);
        curl_free(enc_msg);

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && http_code >= 200 && http_code < 300)
            return SEND_OK;

        if (res != CURLE_OK) {
            /* Network error — transient */
            attempt++;
            sleep((unsigned int)delay);
            delay *= 2;
            continue;
        }

        /* HTTP error */
        if (http_code >= 400 && http_code < 500)
            return SEND_PERMANENT_FAIL; /* Bad request, invalid token, etc. */

        /* 5xx — transient */
        attempt++;
        sleep((unsigned int)delay);
        delay *= 2;
    }

    return SEND_TRANSIENT_FAIL;
}

/* --- Worker thread --- */

static void *push_worker_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&push_mutex);
        while (!push_notify && !push_stop)
            pthread_cond_wait(&push_cond, &push_mutex);

        push_notify = 0;
        int stopping = push_stop;
        pthread_mutex_unlock(&push_mutex);

        /* Process unsent messages */
        while (1) {
            db_result_t *result = NULL;
            int rc = db_execute(push_db,
                "SELECT rowid, timestamp, token, user, ttl, message, title, "
                "html, priority "
                "FROM push WHERE failed = 0 ORDER BY timestamp LIMIT 1",
                NULL, &result);

            if (rc != CUTILS_OK || !result || result->nrows == 0) {
                db_result_free(result);
                break;
            }

            const char *rowid     = result->rows[0][0];
            const char *timestamp = result->rows[0][1];
            const char *token     = result->rows[0][2];
            const char *user      = result->rows[0][3];
            const char *ttl       = result->rows[0][4];
            const char *message   = result->rows[0][5];
            const char *title     = result->rows[0][6];
            int html              = atoi(result->rows[0][7]);
            int priority          = atoi(result->rows[0][8]);

            send_result_t sr = send_one(token, user, title, message,
                                        timestamp, ttl, html, priority);

            if (sr == SEND_OK) {
                const char *del_params[] = { rowid, NULL };
                db_execute_non_query(push_db,
                    "DELETE FROM push WHERE rowid = ?", del_params, NULL);
                log_debug("push sent: %s", title);
            } else if (sr == SEND_PERMANENT_FAIL) {
                const char *fail_params[] = { rowid, NULL };
                db_execute_non_query(push_db,
                    "UPDATE push SET failed = 1 WHERE rowid = ?",
                    fail_params, NULL);
                log_error("push permanently failed: %s", title);
            } else {
                log_warn("push transient failure, will retry: %s", title);
                break; /* Back off, try again next cycle */
            }

            db_result_free(result);
        }

        if (stopping) break;
    }

    return NULL;
}

/* --- Public API --- */

int push_init(cutils_db_t *db, const cutils_config_t *cfg)
{
    push_db = db;
    push_cfg = cfg;
    push_stop = 0;
    push_notify = 0;
    push_running = 1;

    /* Verify creds are configured */
    const char *token = config_get_str(cfg, PUSH_CONFIG_TOKEN);
    const char *user  = config_get_str(cfg, PUSH_CONFIG_USER);
    if (!token || !token[0] || !user || !user[0]) {
        log_info("Pushover not configured, notifications disabled");
        push_running = 0;
        return CUTILS_OK;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int rc = pthread_create(&push_thread, NULL, push_worker_thread, NULL);
    if (rc != 0) {
        push_running = 0;
        return set_error(CUTILS_ERR, "failed to create push worker thread");
    }

    log_info("Pushover notifications enabled");
    return CUTILS_OK;
}

void push_shutdown(void)
{
    if (!push_running) return;
    push_running = 0;

    pthread_mutex_lock(&push_mutex);
    push_stop = 1;
    push_notify = 1;
    pthread_cond_signal(&push_cond);
    pthread_mutex_unlock(&push_mutex);

    pthread_join(push_thread, NULL);
    curl_global_cleanup();
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
    if (!push_db)
        return set_error(CUTILS_ERR_INVALID, "push not initialized");

    const char *token = opts->token;
    const char *user  = opts->user;
    int ttl           = opts->ttl > 0 ? opts->ttl : DEFAULT_TTL;
    int timestamp     = opts->timestamp > 0 ? opts->timestamp : (int)time(NULL);

    if (!token || !token[0])
        token = config_get_str(push_cfg, PUSH_CONFIG_TOKEN);
    if (!user || !user[0])
        user = config_get_str(push_cfg, PUSH_CONFIG_USER);

    if (!token || !token[0] || !user || !user[0])
        return set_error(CUTILS_ERR_CONFIG, "Pushover credentials not configured");

    int rc = split_and_store(opts->title, opts->message,
                             token, user, ttl, timestamp,
                             opts->html, opts->priority);
    if (rc != CUTILS_OK) return rc;

    /* Wake the worker */
    pthread_mutex_lock(&push_mutex);
    push_notify = 1;
    pthread_cond_signal(&push_cond);
    pthread_mutex_unlock(&push_mutex);

    return CUTILS_OK;
}
