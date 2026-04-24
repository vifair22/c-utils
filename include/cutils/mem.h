#ifndef CUTILS_MEM_H
#define CUTILS_MEM_H

#include <stdio.h>
#include <pthread.h>

/* --- Memory safety helpers ---
 *
 * Scoped-cleanup macros built on __attribute__((cleanup(...))) —
 * a GCC/Clang extension that runs a function when a local variable
 * leaves scope, regardless of which return path is taken.
 *
 * Using these eliminates entire classes of bugs:
 *   - leaks on error-return paths
 *   - forgotten mutex unlocks
 *   - forgotten fclose / close(fd)
 *   - use-after-free after an ownership handoff
 *
 * Each macro below has a short docstring with a usage example.
 */

/* --- Cleanup functions (called by the macros — do not invoke directly) --- */

/* Free *p and set *p to NULL. Safe when *p is already NULL.
 * Reads the pointer via memcpy to avoid strict-aliasing concerns when
 * the caller's variable type differs from void * (char *, uint8_t *, etc.).
 *
 * Intended for __attribute__((cleanup(cutils_free_p))) on local variables
 * of pointer type. Use via CUTILS_AUTOFREE. */
void cutils_free_p(void *p);

/* fclose(*f) if *f is non-NULL, then set *f to NULL. Use via CUTILS_AUTOCLOSE. */
void cutils_fclose_p(FILE **f);

/* close(*fd) if *fd >= 0, then set *fd to -1. Use via CUTILS_AUTOCLOSE_FD. */
void cutils_close_fd_p(int *fd);

/* pthread_mutex_unlock(*m) if *m is non-NULL. Use via CUTILS_LOCK_GUARD. */
void cutils_unlock_p(pthread_mutex_t **m);

/* --- Scoped cleanup attribute macros ---
 *
 * Apply to a local variable declaration. The matching cleanup function
 * runs on scope exit — via return, break, goto, or normal fall-through.
 *
 *   int do_thing(void) {
 *       CUTILS_AUTOFREE char *buf = malloc(1024);
 *       if (!buf) return -1;
 *       if (step_one() != 0) return -1;  / * buf freed automatically * /
 *       if (step_two() != 0) return -1;  / * buf freed automatically * /
 *       return 0;                         / * buf freed automatically * /
 *   }
 *
 * CUTILS_AUTOFREE    — free() on any malloc'd pointer (char *, void *, etc.)
 * CUTILS_AUTOCLOSE   — fclose() on a FILE *
 * CUTILS_AUTOCLOSE_FD — close() on a raw file descriptor (int). Initialize to -1.
 */
#define CUTILS_AUTOFREE     __attribute__((cleanup(cutils_free_p)))
#define CUTILS_AUTOCLOSE    __attribute__((cleanup(cutils_fclose_p)))
#define CUTILS_AUTOCLOSE_FD __attribute__((cleanup(cutils_close_fd_p)))

/* --- Scoped mutex lock guard ---
 *
 *   pthread_mutex_t m;
 *
 *   void do_thing(void) {
 *       CUTILS_LOCK_GUARD(&m);
 *       / * m is held for the rest of this block * /
 *       if (error) return;   / * unlocked automatically * /
 *       do_work();
 *       / * unlocked automatically on fall-through * /
 *   }
 *
 * Locks the mutex immediately at the point of use and unlocks it when
 * the enclosing block ends. Each invocation declares a uniquely-named
 * local, so multiple guards may appear in different nested blocks.
 *
 * Do not call pthread_mutex_unlock() manually on the guarded mutex within
 * the guarded scope — the cleanup will run a second unlock on exit. */
#define CUTILS_MEM_CAT_(a, b) a ## b
#define CUTILS_MEM_CAT(a, b)  CUTILS_MEM_CAT_(a, b)

#define CUTILS_LOCK_GUARD(m)                                          \
    pthread_mutex_t *CUTILS_MEM_CAT(_cutils_lg_, __LINE__)            \
        __attribute__((cleanup(cutils_unlock_p))) =                   \
        (pthread_mutex_lock(m), (m))

/* --- Ownership transfer ---
 *
 *   obj_take(CUTILS_MOVE(handle));
 *   / * handle is NULL now — any subsequent use is a null-deref, not a UAF * /
 *
 * Yields the value of the argument and sets it to NULL. Used when
 * ownership of a pointer passes out of the current scope (e.g. pushed
 * into a container that takes ownership). If the caller accidentally
 * touches the variable afterward, they get a null-deref (loud crash)
 * rather than a use-after-free (silent corruption).
 *
 * Note: evaluates its argument twice. Do not pass expressions with
 * side effects (e.g. CUTILS_MOVE(array[i++])). */
#define CUTILS_MOVE(p) \
    __extension__ ({ __typeof__(p) _cutils_mv = (p); (p) = NULL; _cutils_mv; })

/* --- Function attribute conveniences ---
 *
 * Applied to declarations in headers. The compiler uses them to catch
 * misuse at call sites — zero runtime cost.
 *
 * CUTILS_MUST_USE     Warn if the caller discards the return value.
 *                     Apply to status codes, owned pointers, and any
 *                     return where ignoring is a bug.
 *
 * CUTILS_NONNULL(n)   Warn when a literal NULL is passed for the
 *                     listed 1-based parameter positions. Only use
 *                     where the callee truly does not tolerate NULL.
 *
 * CUTILS_MALLOC_FN    The return is a fresh, unaliased pointer. Helps
 *                     the optimizer and the clang static analyzer
 *                     track ownership across function boundaries.
 */
#define CUTILS_MUST_USE     __attribute__((warn_unused_result))
#define CUTILS_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define CUTILS_MALLOC_FN    __attribute__((malloc))

/* --- Intentionally discard a CUTILS_MUST_USE return value ---
 *
 * A plain (void) cast does not silence warn_unused_result in GCC.
 * This macro assigns the result to a local and discards it, which
 * does. Use it only where the caller genuinely cannot act on the
 * failure (cleanup paths, fire-and-forget background work, etc.)
 * and always leave a comment justifying why.
 *
 *   / * rollback failure in a cleanup handler is not actionable * /
 *   CUTILS_UNUSED(db_rollback(db));
 */
#define CUTILS_UNUSED(expr) \
    do { __extension__ __typeof__(expr) _cutils_u = (expr); (void)_cutils_u; } while (0)

#endif
