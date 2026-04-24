#ifndef CUTILS_ERROR_H
#define CUTILS_ERROR_H

#include <stddef.h>

/* --- Return codes ---
 * All c-utils functions return int. OK=0, errors<0.
 * The deepest function that detects an error calls set_error() with a
 * descriptive message. Callers propagate the return code without
 * overwriting the message. */

typedef enum {
    CUTILS_OK              =  0,
    CUTILS_ERR             = -1,  /* generic error */
    CUTILS_ERR_NOMEM       = -2,  /* allocation failure */
    CUTILS_ERR_IO          = -3,  /* file/network I/O */
    CUTILS_ERR_DB          = -4,  /* database operation failed */
    CUTILS_ERR_CONFIG      = -5,  /* config parse/validation error */
    CUTILS_ERR_MIGRATE     = -6,  /* migration failed */
    CUTILS_ERR_NOT_FOUND   = -7,  /* key/record not found */
    CUTILS_ERR_INVALID     = -8,  /* invalid argument or state */
    CUTILS_ERR_EXISTS      = -9,  /* duplicate/collision */
    CUTILS_ERR_JSON        = -10, /* JSON parse/validation error */
} cutils_err_t;

/* Maximum length of the thread-local error message buffer */
#define CUTILS_ERR_BUFSZ 512

/* Set the thread-local error message.
 * Call from the deepest function that detects the error.
 *
 *   set_error(CUTILS_ERR_IO, "failed to open %s", path);
 *   return CUTILS_ERR_IO;
 */
#define set_error(code, ...) \
    cutils_set_error_((code), __VA_ARGS__)

/* Variant that appends ": <strerror(errno)>" to the message.
 * Use after a syscall failure.
 *
 *   set_error_errno(CUTILS_ERR_IO, "open(%s)", path);
 */
#define set_error_errno(code, ...) \
    cutils_set_error_errno_((code), __VA_ARGS__)

/* Retrieve the last error message for the calling thread.
 * Returns "" if no error has been set. */
const char *cutils_get_error(void);

/* Clear the thread-local error state. */
void cutils_clear_error(void);

/* Return a short name for an error code ("OK", "ERR_IO", etc.) */
const char *cutils_err_name(cutils_err_t code);

/* --- Internal — called by macros, do not call directly --- */
int cutils_set_error_(int code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int cutils_set_error_errno_(int code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
