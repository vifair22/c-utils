#include "cutils/version.h"

#ifndef CUTILS_VERSION_STRING
#  error "CUTILS_VERSION_STRING must be defined by the build system"
#endif

const char *cutils_version(void)
{
    return CUTILS_VERSION_STRING;
}
