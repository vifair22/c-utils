#ifndef CUTILS_PUSH_H
#define CUTILS_PUSH_H

#include "cutils/mem.h"

/* --- Pushover notification subsystem ---
 *
 * DB-persisted notification queue with background worker thread.
 * Messages survive crashes — the worker picks up unsent messages on restart.
 * Retry with exponential backoff (3 attempts).
 * Messages > 1024 chars are split on newline boundaries. */

/* Forward declarations */
typedef struct cutils_db cutils_db_t;
typedef struct cutils_config cutils_config_t;

/* Initialize the push subsystem.
 * Reads token/user from config keys "pushover.token" and "pushover.user".
 * Starts the background worker thread.
 * db must be open with push table created (via migrations).
 * Returns CUTILS_OK on success. */
CUTILS_MUST_USE
int push_init(cutils_db_t *db, const cutils_config_t *cfg);

/* Shut down the push worker. Drains pending messages first. */
void push_shutdown(void);

/* Send a notification. The message is persisted to DB immediately
 * and delivered asynchronously by the worker thread.
 * Returns CUTILS_OK on successful enqueue. */
CUTILS_MUST_USE
int push_send(const char *title, const char *message);

/* Builder pattern for notifications with overrides. */
typedef struct {
    const char *title;
    const char *message;
    int         ttl;        /* seconds, default 86400 (24h) */
    int         timestamp;  /* unix epoch, default now */
    const char *token;      /* override config token */
    const char *user;       /* override config user */
    int         priority;   /* -2..2 (0 = normal, see PUSH_PRIORITY_*) */
    int         html;       /* 1 = enable HTML in message body */
} push_opts_t;

/* Pushover priority levels */
#define PUSH_PRIORITY_LOWEST  (-2)  /* no notification/alert */
#define PUSH_PRIORITY_LOW     (-1)  /* quiet, no sound/vibration */
#define PUSH_PRIORITY_NORMAL    0   /* default */
#define PUSH_PRIORITY_HIGH      1   /* bypasses quiet hours */
#define PUSH_PRIORITY_EMERGENCY 2   /* repeats until acknowledged */

/* Send with explicit options. Fields set to 0/NULL use defaults. */
CUTILS_MUST_USE
int push_send_opts(const push_opts_t *opts);

/* Config keys that c-utils registers for Pushover.
 * The consuming app decides store (file or DB) via cutils_optional_store. */
#define PUSH_CONFIG_TOKEN "pushover.token"
#define PUSH_CONFIG_USER  "pushover.user"

#endif
