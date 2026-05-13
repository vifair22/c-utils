#include "cutils/error_loop.h"
#include "cutils/log.h"
#include "cutils/mem.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct error_loop {
    int             threshold;
    int             cooldown_sec;
    error_loop_fn   on_loop;
    void           *userdata;
    char           *last_identity;
    char           *last_raw;
    int             count;
    time_t          suppressed_until;
    pthread_mutex_t mutex;       /* protects last_*, count, suppressed_until */
};

/* --- Normalization ---
 *
 * Strip variable parts of error messages so the same logical error
 * matches across iterations. Each pattern is a small "try this
 * shape at position p" function that returns the in-place
 * replacement's length (so the dispatcher can advance past it), or
 * 0 if the pattern doesn't match. The dispatcher in
 * normalize_inplace tries each in declared order and either consumes
 * the replacement length or advances one byte.
 *
 * Adding a new pattern is a self-contained edit: write one
 * try_replace_X that probes at p, and append it to the dispatch
 * order in normalize_inplace. No edits to a shared megafunction. */

/* Replace the run starting at p with `repl` (length rlen), shifting
 * the tail down. Returns rlen so callers can do p += try_replace_...
 * cleanly. The tail's strlen(q) accounts for the new NUL position. */
static size_t replace_run(char *p, const char *q, const char *repl, size_t rlen)
{
    memmove(p + rlen, q, strlen(q) + 1);
    memcpy(p, repl, rlen);
    return rlen;
}

/* UUID: 8-4-4-4-12 hex with hyphens. */
static size_t try_replace_uuid(char *p)
{
    if (!isxdigit((unsigned char)p[0]) || strlen(p) < 36)
        return 0;
    static const int groups[] = { 8, 4, 4, 4, 12 };
    const char *q = p;
    for (int g = 0; g < 5; g++) {
        for (int c = 0; c < groups[g]; c++) {
            if (!isxdigit((unsigned char)*q)) return 0;
            q++;
        }
        if (g < 4) {
            if (*q != '-') return 0;
            q++;
        }
    }
    /* Trailing char must not extend the run alphanumerically — that
     * would mean the UUID is a substring of a longer token. */
    if (isalnum((unsigned char)*q))
        return 0;
    return replace_run(p, q, "<uuid>", 6);
}

/* "0x" prefix followed by at least 4 hex digits. */
static size_t try_replace_hex_addr(char *p)
{
    if (p[0] != '0' || p[1] != 'x' || !isxdigit((unsigned char)p[2]))
        return 0;
    const char *q = p + 2;
    while (isxdigit((unsigned char)*q)) q++;
    if ((size_t)(q - p) < 4)
        return 0;
    return replace_run(p, q, "<addr>", 6);
}

/* ISO 8601 timestamp: YYYY-MM-DD[T ]HH:MM:SS, with optional
 * fractional seconds. */
static size_t try_replace_timestamp(char *p)
{
    if (!isdigit((unsigned char)p[0]) || strlen(p) < 19)
        return 0;
    if (p[4] != '-' || p[7] != '-' ||
        (p[10] != 'T' && p[10] != ' ') ||
        p[13] != ':' || p[16] != ':')
        return 0;
    const char *q = p + 19;
    if (*q == '.') { q++; while (isdigit((unsigned char)*q)) q++; }
    return replace_run(p, q, "<timestamp>", 11);
}

/* 8+ contiguous hex characters NOT followed by an alpha (which would
 * make this part of a longer identifier). */
static size_t try_replace_long_hex(char *p)
{
    if (!isxdigit((unsigned char)p[0]))
        return 0;
    const char *q = p;
    while (isxdigit((unsigned char)*q)) q++;
    if ((size_t)(q - p) < 8 || isalpha((unsigned char)*q))
        return 0;
    return replace_run(p, q, "<hex>", 5);
}

static void normalize_inplace(char *buf)
{
    /* Try patterns in declared order at each position. UUID first
     * because its trailing-group hex run would otherwise be eaten by
     * the long-hex pattern; hex-addr before long-hex for the same
     * reason w.r.t the "0x" prefix. */
    char *p = buf;
    while (*p) {
        size_t adv = try_replace_uuid(p);
        if (!adv) adv = try_replace_hex_addr(p);
        if (!adv) adv = try_replace_timestamp(p);
        if (!adv) adv = try_replace_long_hex(p);
        p += adv ? adv : 1;
    }
}

static char *make_identity(const char *raw)
{
    char *copy = strdup(raw);
    if (!copy) return NULL;
    normalize_inplace(copy);
    return copy;
}

/* --- Default callback --- */

static void default_callback(const char *message, int count, void *userdata)
{
    (void)userdata;
    log_error("repeating error detected (%dx): %s", count, message);
}

/* --- Public API --- */

error_loop_t *error_loop_create(int threshold, int cooldown_sec,
                                error_loop_fn on_loop, void *userdata)
{
    error_loop_t *det = calloc(1, sizeof(*det));
    if (!det) return NULL;

    /* LCOV_EXCL_START — pthread_mutex_init only fails on EAGAIN / EINVAL;
     * not reachable at this call site (default attrs, ample resources). */
    if (pthread_mutex_init(&det->mutex, NULL) != 0) {
        free(det);
        return NULL;
    }
    /* LCOV_EXCL_STOP */

    det->threshold = threshold > 0 ? threshold : 5;
    det->cooldown_sec = cooldown_sec;
    det->on_loop = on_loop ? on_loop : default_callback;
    det->userdata = userdata;
    det->count = 0;
    det->suppressed_until = 0;

    return det;
}

void error_loop_free(error_loop_t *det)
{
    if (!det) return;
    free(det->last_identity);
    free(det->last_raw);
    pthread_mutex_destroy(&det->mutex);
    free(det);
}

void error_loop_free_p(error_loop_t **det)
{
    if (*det) {
        error_loop_free(*det);
        *det = NULL;
    }
}

/* Action decided under the lock; the actual log/callback fires after we
 * release the mutex so a callback that re-enters error_loop_* doesn't
 * self-deadlock on the non-recursive mutex. */
typedef enum {
    ACT_NONE,
    ACT_LOG_NORMAL,
    ACT_FIRE_THRESHOLD,
} report_action_t;

void error_loop_report(error_loop_t *det, const char *fmt, ...)
{
    char raw[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);

    CUTILS_AUTOFREE char *identity = make_identity(raw);
    if (!identity) return;

    report_action_t action = ACT_NONE;
    int snapshot_count     = 0;

    {
        CUTILS_LOCK_GUARD(&det->mutex);

        /* Same error as last time? */
        if (det->last_identity && strcmp(identity, det->last_identity) == 0) {
            det->count++;
            free(det->last_raw);
            det->last_raw = strdup(raw);
        } else {
            free(det->last_identity);
            free(det->last_raw);
            det->last_identity   = CUTILS_MOVE(identity);  /* transfer — cleanup no-op */
            det->last_raw        = strdup(raw);
            det->count           = 1;
            det->suppressed_until = 0;
        }

        /* Decide the action while we still hold consistent state. */
        time_t now = time(NULL);
        if (now >= det->suppressed_until) {
            if (det->count < det->threshold) {
                action = ACT_LOG_NORMAL;
            } else if (det->count == det->threshold) {
                action = ACT_FIRE_THRESHOLD;
                if (det->cooldown_sec > 0)
                    det->suppressed_until = now + det->cooldown_sec;
            }
            snapshot_count = det->count;
        }
        /* count > threshold and not in cooldown: re-fire at threshold intervals */
    }

    /* on_loop and userdata are immutable after error_loop_create, so reading
     * them outside the lock is safe. The raw stack buffer is also local. */
    if (action == ACT_LOG_NORMAL) {
        log_error("main loop: %s", raw);
    } else if (action == ACT_FIRE_THRESHOLD) {
        log_error("main loop: repeating error (%dx): %s", snapshot_count, raw);
        det->on_loop(raw, snapshot_count, det->userdata);
    }
}

void error_loop_success(error_loop_t *det)
{
    CUTILS_LOCK_GUARD(&det->mutex);
    free(det->last_identity);
    free(det->last_raw);
    det->last_identity = NULL;
    det->last_raw = NULL;
    det->count = 0;
    det->suppressed_until = 0;
}
