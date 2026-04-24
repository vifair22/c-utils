#ifndef CUTILS_MEM_H
#define CUTILS_MEM_H

/* --- Memory utilities ---
 *
 * Cleanup helpers for use with __attribute__((cleanup(...))), and
 * (future home for) safer allocator wrappers.
 *
 * Usage:
 *   char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
 *   buf = strdup("hello");
 *   / * buf is free()'d automatically when the variable leaves scope,
 *       regardless of which return path is taken * /
 */

/* Cleanup helper for any malloc'd pointer variable.
 * Frees *p and sets *p to NULL. Safe when *p is already NULL.
 *
 * Intended for __attribute__((cleanup(cutils_free_p))) on local variables
 * of pointer type (char *, void *, uint8_t *, etc.). The cleanup attribute
 * passes the address of the variable, so any pointer-to-pointer works. */
void cutils_free_p(void *p);

#endif
