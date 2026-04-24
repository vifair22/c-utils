#include "cutils/log.h"
#include "cutils/db.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- ANSI colors --- */

#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RED     "\033[31m"

/* --- Log queue entry --- */

typedef struct log_entry {
    char              timestamp[32];
    char              level[8];
    char              func[128];
    char             *message;
    struct log_entry *next;
} log_entry_t;

/* --- Stream callbacks --- */

#define MAX_STREAMS 8

typedef struct {
    log_stream_fn fn;
    void         *userdata;
    int           active;
} log_stream_slot_t;

/* --- Module state --- */

static cutils_db_t     *log_db           = NULL;
static log_level_t      log_min_level    = LOG_INFO;
static int              log_retention    = 0;
static int              log_running      = 0;

/* Writer thread */
static pthread_t        log_thread;
static pthread_mutex_t  log_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   log_cond      = PTHREAD_COND_INITIALIZER;
static log_entry_t     *log_queue_head = NULL;
static log_entry_t     *log_queue_tail = NULL;
static int              log_stop       = 0;

/* Stream callbacks */
static log_stream_slot_t log_streams[MAX_STREAMS];
static pthread_mutex_t   stream_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- Helpers --- */

static void get_timestamp(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = (int)strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
    if (n > 0 && (size_t)n < len - 4)
        snprintf(buf + n, len - (size_t)n, ".%03ld", ts.tv_nsec / 1000000);
}

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG:   return "debug";
    case LOG_INFO:    return "info";
    case LOG_WARNING: return "warning";
    case LOG_ERROR:   return "error";
    }
    return "unknown";
}

static const char *level_color(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG:   return COLOR_CYAN;
    case LOG_INFO:    return "";
    case LOG_WARNING: return COLOR_YELLOW;
    case LOG_ERROR:   return COLOR_RED;
    }
    return "";
}

/* --- Console output --- */

static void print_to_console(const char *timestamp, log_level_t level,
                             const char *func, const char *message)
{
    FILE *out = (level >= LOG_ERROR) ? stderr : stdout;
    const char *color = level_color(level);
    int has_color = color[0] != '\0';

    fprintf(out, COLOR_BLUE "%s" COLOR_RESET " " COLOR_BOLD "[%s]" COLOR_RESET " ",
            timestamp, func);

    if (has_color)
        fprintf(out, "%s%s" COLOR_RESET "\n", color, message);
    else
        fprintf(out, "%s\n", message);
}

/* --- Stream fan-out --- */

static void fire_streams(const char *timestamp, const char *level_s,
                         const char *func, const char *message)
{
    CUTILS_LOCK_GUARD(&stream_mutex);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (log_streams[i].active && log_streams[i].fn)
            log_streams[i].fn(timestamp, level_s, func, message,
                              log_streams[i].userdata);
    }
}

/* --- DB writer thread --- */

static void write_entry_to_db(const log_entry_t *entry)
{
    if (!log_db) return;

    const char *params[] = {
        entry->timestamp, entry->level, entry->func, entry->message, NULL
    };
    /* Writer thread is fire-and-forget — a DB insert failure here would
     * recurse if we tried to log it. Drop silently. */
    CUTILS_UNUSED(db_execute_non_query(log_db,
        "INSERT INTO logs (timestamp, level, function, message) "
        "VALUES (?, ?, ?, ?)",
        params, NULL));
}

static void do_cleanup(void)
{
    if (!log_db || log_retention <= 0) return;

    char cutoff[32];
    time_t now = time(NULL);
    time_t threshold = now - (time_t)log_retention * 86400;
    struct tm tm;
    localtime_r(&threshold, &tm);
    strftime(cutoff, sizeof(cutoff), "%Y-%m-%d %H:%M:%S", &tm);

    const char *params[] = { cutoff, NULL };
    int affected = 0;
    /* Cleanup runs on a background thread — caller has no handle to fail. */
    CUTILS_UNUSED(db_execute_non_query(log_db,
        "DELETE FROM logs WHERE timestamp < ?", params, &affected));

    if (affected > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "log cleanup: removed %d entries older than %d days",
                 affected, log_retention);

        char ts_now[32];
        get_timestamp(ts_now, sizeof(ts_now));

        const char *ins_params[] = { ts_now, "info", "log_cleanup", msg, NULL };
        CUTILS_UNUSED(db_execute_non_query(log_db,
            "INSERT INTO logs (timestamp, level, function, message) "
            "VALUES (?, ?, ?, ?)",
            ins_params, NULL));
    }
}

static void *log_writer_thread(void *arg)
{
    (void)arg;

    time_t last_cleanup = 0;

    while (1) {
        /* Drain the queue while holding the mutex; write outside the
         * critical section so DB work doesn't block enqueuers. */
        log_entry_t *batch;
        int          stopping;
        {
            CUTILS_LOCK_GUARD(&log_mutex);
            while (!log_queue_head && !log_stop)
                pthread_cond_wait(&log_cond, &log_mutex);

            batch          = log_queue_head;
            log_queue_head = NULL;
            log_queue_tail = NULL;
            stopping       = log_stop;
        }

        /* Write batch to DB */
        while (batch) {
            log_entry_t *entry = batch;
            batch = entry->next;
            write_entry_to_db(entry);
            free(entry->message);
            free(entry);
        }

        /* Periodic cleanup (once per day) */
        time_t now = time(NULL);
        if (log_retention > 0 && now - last_cleanup >= 86400) {
            do_cleanup();
            last_cleanup = now;
        }

        if (stopping) break;
    }

    return NULL;
}

/* --- Enqueue --- */

static void enqueue_entry(const char *timestamp, log_level_t level,
                          const char *func, const char *message)
{
    CUTILS_AUTOFREE log_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return;

    snprintf(entry->timestamp, sizeof(entry->timestamp), "%s", timestamp);
    snprintf(entry->level, sizeof(entry->level), "%s", level_str(level));
    snprintf(entry->func, sizeof(entry->func), "%s", func);
    entry->message = strdup(message);
    entry->next = NULL;

    /* If strdup failed, return with CUTILS_AUTOFREE handling entry. */
    if (!entry->message) return;

    CUTILS_LOCK_GUARD(&log_mutex);
    /* Transfer ownership into the queue — CUTILS_AUTOFREE becomes a no-op. */
    log_entry_t *q = CUTILS_MOVE(entry);
    if (log_queue_tail)
        log_queue_tail->next = q;
    else
        log_queue_head = q;
    log_queue_tail = q;
    pthread_cond_signal(&log_cond);
}

/* --- Public API --- */

int log_init(cutils_db_t *db, log_level_t level, int retention_days)
{
    log_db = db;
    log_min_level = level;
    log_retention = retention_days;
    log_stop = 0;
    log_running = 1;

    memset(log_streams, 0, sizeof(log_streams));

    if (db) {
        int rc = pthread_create(&log_thread, NULL, log_writer_thread, NULL);
        if (rc != 0)
            return set_error(CUTILS_ERR, "failed to create log writer thread");
    }

    return CUTILS_OK;
}

void log_shutdown(void)
{
    if (!log_running) return;
    log_running = 0;

    if (log_db) {
        {
            CUTILS_LOCK_GUARD(&log_mutex);
            log_stop = 1;
            pthread_cond_signal(&log_cond);
        }

        pthread_join(log_thread, NULL);
    }

    /* Free any remaining entries */
    log_entry_t *entry = log_queue_head;
    while (entry) {
        log_entry_t *next = entry->next;
        free(entry->message);
        free(entry);
        entry = next;
    }
    log_queue_head = NULL;
    log_queue_tail = NULL;
}

void log_set_level(log_level_t level)
{
    log_min_level = level;
}

log_level_t log_get_level(void)
{
    return log_min_level;
}

int log_stream_register(log_stream_fn fn, void *userdata)
{
    CUTILS_LOCK_GUARD(&stream_mutex);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!log_streams[i].active) {
            log_streams[i].fn = fn;
            log_streams[i].userdata = userdata;
            log_streams[i].active = 1;
            return i;
        }
    }
    return -1;
}

void log_stream_unregister(int handle)
{
    if (handle < 0 || handle >= MAX_STREAMS) return;
    CUTILS_LOCK_GUARD(&stream_mutex);
    log_streams[handle].active = 0;
    log_streams[handle].fn = NULL;
    log_streams[handle].userdata = NULL;
}

void log_write(log_level_t level, const char *func, const char *fmt, ...)
{
    if (level < log_min_level) return;

    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    char message[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    /* Console output (synchronous) */
    print_to_console(timestamp, level, func, message);

    /* Stream callbacks */
    fire_streams(timestamp, level_str(level), func, message);

    /* DB persistence (async) */
    if (log_db && log_running)
        enqueue_entry(timestamp, level, func, message);
}
