#include "cutils/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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

void cutils_fclose_p(FILE **f)
{
    if (*f) {
        fclose(*f);
        *f = NULL;
    }
}

void cutils_close_fd_p(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

void cutils_unlock_p(pthread_mutex_t **m)
{
    if (*m)
        pthread_mutex_unlock(*m);
}
