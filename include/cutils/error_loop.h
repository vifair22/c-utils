#ifndef CUTILS_ERROR_LOOP_H
#define CUTILS_ERROR_LOOP_H

/* --- Error loop detector ---
 *
 * Tracks consecutive identical errors in a main loop. When the same error
 * repeats beyond a threshold, fires a callback and enters cooldown to
 * suppress log spam.
 *
 * Error messages are normalized (UUIDs, timestamps, hex addresses stripped)
 * so that variable parts don't prevent matching.
 *
 * Usage:
 *   error_loop_t *detector = error_loop_create(5, 300, my_callback, NULL);
 *   while (running) {
 *       int rc = do_work();
 *       if (rc != 0)
 *           error_loop_report(detector, "do_work failed: %s", error_msg);
 *       else
 *           error_loop_success(detector);
 *   }
 *   error_loop_free(detector);
 */

#include <stddef.h>

#include "cutils/mem.h"

/* Callback fired when threshold is reached.
 * message is the raw (un-normalized) error message.
 * count is the number of consecutive occurrences. */
typedef void (*error_loop_fn)(const char *message, int count, void *userdata);

/* Opaque detector handle */
typedef struct error_loop error_loop_t;

/* Create a detector.
 * threshold: consecutive identical errors before triggering callback.
 * cooldown_sec: seconds to suppress after triggering (0 = no cooldown).
 * on_loop: callback, or NULL for default (logs at error level).
 * userdata: passed to callback. */
CUTILS_MUST_USE CUTILS_MALLOC_FN
error_loop_t *error_loop_create(int threshold, int cooldown_sec,
                                error_loop_fn on_loop, void *userdata);

/* Free a detector. */
void error_loop_free(error_loop_t *det);

/* Cleanup helper for __attribute__((cleanup(...))).
 * Frees *det and sets *det to NULL. Use via CUTILS_AUTO_ERRLOOP. */
void error_loop_free_p(error_loop_t **det);

/* Scoped cleanup for error_loop_t *:
 *
 *   CUTILS_AUTO_ERRLOOP error_loop_t *det = error_loop_create(5, 300, NULL, NULL);
 *   / * det is freed automatically on scope exit * /
 */
#define CUTILS_AUTO_ERRLOOP __attribute__((cleanup(error_loop_free_p)))

/* Report an error from the main loop.
 * Below threshold: logs normally.
 * At threshold: fires callback, starts cooldown.
 * During cooldown: suppressed (silent). */
void error_loop_report(error_loop_t *det, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Reset the detector after a successful iteration. */
void error_loop_success(error_loop_t *det);

#endif
