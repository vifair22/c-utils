#include "cutils/mem.h"

#include <stdlib.h>
#include <string.h>

void cutils_free_p(void *p)
{
    /* Read the pointer value stored at *p, free it, and write NULL back.
     * Uses memcpy to avoid strict-aliasing concerns when the caller's
     * variable type differs from void * (e.g. char *, uint8_t *). */
    void *ptr;
    memcpy(&ptr, p, sizeof(ptr));
    free(ptr);
    ptr = NULL;
    memcpy(p, &ptr, sizeof(ptr));
}
