#ifndef CUTILS_LOG_H
#define CUTILS_LOG_H

#include "cutils/mem.h"

/* --- Logging subsystem ---
 *
 * Dual output: stdout (info/debug/warning) + stderr (error).
 * Colored console output via ANSI escape codes.
 * Async SQLite persistence via background writer thread.
 * Multiple stream callback registrations for live log fan-out.
 * Configurable retention with automatic cleanup. */

/* Log levels */
typedef enum {
    LOG_DEBUG   = 10,
    LOG_INFO    = 20,
    LOG_WARNING = 30,
    LOG_ERROR   = 40,
} log_level_t;

/* Stream callback — registered by the app for live log fan-out.
 * Called on every log event that passes the level filter. */
typedef void (*log_stream_fn)(const char *timestamp, const char *level,
                              const char *func, const char *message,
                              void *userdata);

/* Forward declarations */
typedef struct cutils_db cutils_db_t;

/* Initialize the logging subsystem.
 * db may be NULL to disable DB persistence.
 * retention_days controls auto-cleanup (0 = no cleanup).
 * level sets the minimum log level for output. */
CUTILS_MUST_USE
int log_init(cutils_db_t *db, log_level_t level, int retention_days);

/* Shut down the logging subsystem. Drains pending writes. */
void log_shutdown(void);

/* Set the minimum log level at runtime. */
void log_set_level(log_level_t level);

/* Get the current log level. */
log_level_t log_get_level(void);

/* Register a stream callback. Returns a handle for unregistering.
 * userdata is passed through to the callback. Max 8 callbacks. */
int log_stream_register(log_stream_fn fn, void *userdata);

/* Unregister a stream callback by handle. */
void log_stream_unregister(int handle);

/* --- Logging macros ---
 * These capture __func__ automatically. Use these, not log_write() directly. */

#define log_debug(...) \
    log_write(LOG_DEBUG, __func__, __VA_ARGS__)

#define log_info(...) \
    log_write(LOG_INFO, __func__, __VA_ARGS__)

#define log_warn(...) \
    log_write(LOG_WARNING, __func__, __VA_ARGS__)

#define log_error(...) \
    log_write(LOG_ERROR, __func__, __VA_ARGS__)

/* Internal — called by macros */
void log_write(log_level_t level, const char *func, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
