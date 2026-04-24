#include "cutils/error.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

/* Thread-local error state */
static _Thread_local char  err_buf[CUTILS_ERR_BUFSZ];
static _Thread_local int   err_set = 0;

int cutils_set_error_(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof(err_buf), fmt, ap);
    va_end(ap);
    err_set = 1;
    return code;
}

int cutils_set_error_errno_(int code, const char *fmt, ...)
{
    int saved_errno = errno;
    char msg[CUTILS_ERR_BUFSZ];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (n > 0 && (size_t)n < sizeof(msg) - 2) {
        snprintf(msg + n, sizeof(msg) - (size_t)n, ": %s", strerror(saved_errno));
    }

    memcpy(err_buf, msg, sizeof(err_buf));
    err_buf[sizeof(err_buf) - 1] = '\0';
    err_set = 1;
    return code;
}

const char *cutils_get_error(void)
{
    return err_set ? err_buf : "";
}

void cutils_clear_error(void)
{
    err_buf[0] = '\0';
    err_set = 0;
}

const char *cutils_err_name(cutils_err_t code)
{
    switch (code) {
    case CUTILS_OK:            return "OK";
    case CUTILS_ERR:           return "ERR";
    case CUTILS_ERR_NOMEM:     return "ERR_NOMEM";
    case CUTILS_ERR_IO:        return "ERR_IO";
    case CUTILS_ERR_DB:        return "ERR_DB";
    case CUTILS_ERR_CONFIG:    return "ERR_CONFIG";
    case CUTILS_ERR_MIGRATE:   return "ERR_MIGRATE";
    case CUTILS_ERR_NOT_FOUND: return "ERR_NOT_FOUND";
    case CUTILS_ERR_INVALID:   return "ERR_INVALID";
    case CUTILS_ERR_EXISTS:    return "ERR_EXISTS";
    case CUTILS_ERR_JSON:      return "ERR_JSON";
    }
    return "ERR_UNKNOWN";
}
