#ifndef CUTILS_LOG_H
#define CUTILS_LOG_H

#include "cutils/mem.h"

/* --- Logging subsystem ---
 *
 * Default console output: stdout (info/debug/warning) + stderr (error),
 * colored with ANSI escape codes when both streams are TTYs and NO_COLOR
 * is unset.
 *
 * Systemd mode (see log_set_systemd_mode): all output to stdout, no
 * timestamp, no color, RFC 5424 priority prefix per line.
 *
 * Async SQLite persistence via background writer thread.
 * Multiple stream callback registrations for live log fan-out.
 * Configurable retention with automatic cleanup.
 *
 * Quick reference:
 *
 *   // After appguard_init() returns, just use the macros:
 *   log_debug("started worker %d", id);
 *   log_info("processed %zu records", n);
 *   log_warn("retry %d/%d on %s", attempt, max, host);
 *   log_error("failed: %s", cutils_get_error());
 *
 *   // Adjust level at runtime (default LOG_INFO):
 *   log_set_level(LOG_DEBUG);
 *
 *   // Live fan-out to a WebSocket / metrics endpoint / etc:
 *   int h = log_stream_register(my_callback, my_ctx);
 *   ...
 *   log_stream_unregister(h);   // keep my_ctx alive briefly after — see below */

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

/* Enable or disable systemd-native console output. When enabled, the
 * console writer:
 *   - drops the timestamp (journald stamps each line on receive)
 *   - drops ANSI color (journald stores escapes as literal bytes —
 *     garbage in --no-pager / grep / log forwarders)
 *   - sends every line to stdout (no stream split — stream-default
 *     priority becomes redundant)
 *   - prefixes each line with <N>, the RFC 5424 syslog priority,
 *     which journald parses and strips: 7=DEBUG, 6=INFO, 4=WARNING,
 *     3=ERROR
 *
 * Format: <N>[function] message\n
 *
 * AppGuard calls this automatically when log_systemd_autodetect is set
 * in appguard_config_t and JOURNAL_STREAM is present in the environment.
 * Direct callers can flip it for testing or non-AppGuard integrations. */
void log_set_systemd_mode(int enabled);

/* Returns nonzero if systemd mode is active. */
int log_get_systemd_mode(void);

/* Register a stream callback. Returns a handle for unregistering.
 * userdata is passed through to the callback. Max 8 callbacks.
 *
 * Callbacks fire from the calling thread of every log_* call that
 * passes the level filter, with the registry lock NOT held. This
 * means a callback may itself call log_* without self-deadlocking,
 * and a slow callback does not block other producers — but it also
 * means log_stream_unregister can return while a concurrent fan-out
 * still holds a snapshot of the just-unregistered slot. Apps that
 * unregister at runtime must keep userdata alive until any in-flight
 * log_* calls on other threads have returned. */
int log_stream_register(log_stream_fn fn, void *userdata);

/* Unregister a stream callback by handle. See log_stream_register
 * for the userdata-lifetime caveat under concurrent log_* calls. */
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
