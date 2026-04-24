#ifndef CUTILS_JSON_H
#define CUTILS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cutils/mem.h"

/* --- JSON wrapper ---
 *
 * Type-safe JSON request parsing and response building.
 * Callers never see the underlying JSON library (currently cJSON).
 * Do not include cJSON headers from application code.
 *
 * Core invariant: no function in this header returns a pointer into
 * internal storage. Strings are always owned copies (free with free()).
 * Nested access is by dot-notation path. Array iteration uses a
 * stack-scoped iterator that cannot outlive its enclosing scope.
 *
 * Errors: every function returns an int status (CUTILS_OK or
 * CUTILS_ERR_JSON). On failure, the thread-local error buffer is set
 * with field name and reason — retrieve with cutils_get_error().
 *
 * Usage sketch:
 *
 *   cutils_json_req_t *req  __attribute__((cleanup(json_req_free_p))) = NULL;
 *   char              *ip   __attribute__((cleanup(cutils_free_p)))  = NULL;
 *   uint16_t           port;
 *
 *   if (json_req_parse(body, body_len, &req) != CUTILS_OK) return -1;
 *   if (json_req_get_str(req, "ip", &ip) != CUTILS_OK) return -1;
 *   if (json_req_get_u32(req, "port", (uint32_t *)&port, 1, 65535) != CUTILS_OK)
 *       return -1;
 *   / * use ip, port — all frees happen automatically on scope exit * /
 */

/* Opaque handles */
typedef struct cutils_json_req  cutils_json_req_t;
typedef struct cutils_json_resp cutils_json_resp_t;

/* ============================================================ */
/* Request side — parse and read                                */
/* ============================================================ */

/* Parse a JSON object from a buffer. Input buffer is not retained;
 * the request handle owns its own copy of the parsed data.
 * Returns CUTILS_OK on success, CUTILS_ERR_JSON on parse failure. */
CUTILS_MUST_USE
int json_req_parse(const char *buf, size_t len, cutils_json_req_t **out);

/* Free a request handle. Safe to call with NULL. */
void json_req_free(cutils_json_req_t *req);

/* --- Scalar getters — paths are dot-notation ("network.dns.primary") --- */

/* Extract a required string. On success, *out is a malloc'd copy (caller frees).
 * On failure, *out is NULL. */
CUTILS_MUST_USE
int json_req_get_str (const cutils_json_req_t *req, const char *path, char **out);

/* Same as json_req_get_str but tolerant of absence: if the path is missing,
 * succeeds with *out = NULL. Type mismatch still fails. */
CUTILS_MUST_USE
int json_req_get_str_opt(const cutils_json_req_t *req, const char *path, char **out);

/* Numeric getters — min/max are required. Pass the type's full range for
 * unbounded fields (explicit opt-out). Fails on absence, type mismatch,
 * or out-of-range value. */
CUTILS_MUST_USE
int json_req_get_u32 (const cutils_json_req_t *req, const char *path,
                      uint32_t *out, uint32_t min, uint32_t max);
CUTILS_MUST_USE
int json_req_get_u64 (const cutils_json_req_t *req, const char *path,
                      uint64_t *out, uint64_t min, uint64_t max);
CUTILS_MUST_USE
int json_req_get_i32 (const cutils_json_req_t *req, const char *path,
                      int32_t *out, int32_t min, int32_t max);
CUTILS_MUST_USE
int json_req_get_i64 (const cutils_json_req_t *req, const char *path,
                      int64_t *out, int64_t min, int64_t max);
CUTILS_MUST_USE
int json_req_get_f64 (const cutils_json_req_t *req, const char *path,
                      double *out, double min, double max);

/* Boolean getter. Fails on absence or type mismatch. */
CUTILS_MUST_USE
int json_req_get_bool(const cutils_json_req_t *req, const char *path, bool *out);

/* --- Presence / structural queries --- */

/* True if the path resolves to a value (of any type, including null). */
bool json_req_has    (const cutils_json_req_t *req, const char *path);

/* True if the path resolves to a JSON null. False if absent or non-null. */
bool json_req_is_null(const cutils_json_req_t *req, const char *path);

/* Length of the array at path. Returns CUTILS_ERR_JSON if absent or not an array. */
CUTILS_MUST_USE
int  json_req_array_len(const cutils_json_req_t *req, const char *path, size_t *out);

/* --- Array iteration ---
 *
 * Stack-allocate the iterator, initialize with _begin, step with _next
 * until it returns false. All _get_ calls on the iterator return owned
 * copies, same as the top-level getters. The iterator must not outlive
 * the request handle.
 *
 *   cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
 *   if (json_iter_begin(req, "ups_list", &it) != CUTILS_OK) return -1;
 *   while (json_iter_next(&it)) {
 *       char *name __attribute__((cleanup(cutils_free_p))) = NULL;
 *       if (json_iter_get_str(&it, "name", &name) != CUTILS_OK) return -1;
 *       / * ... * /
 *   }
 */
typedef struct {
    /* Private — do not read or modify these fields. */
    void  *_array;
    void  *_current;
    size_t _index;
    size_t _len;
} cutils_json_iter_t;

/* Initialize iterator over the array at path. Fails if absent or not an array. */
CUTILS_MUST_USE
int  json_iter_begin(const cutils_json_req_t *req, const char *path,
                     cutils_json_iter_t *iter);

/* Advance to the next element. Returns true if an element is now current,
 * false when the end of the array is reached. */
bool json_iter_next (cutils_json_iter_t *iter);

/* Release any iterator-internal state. Safe to call on an exhausted iterator.
 * Invoked automatically via the cleanup attribute pattern. */
void json_iter_end  (cutils_json_iter_t *iter);

/* Element getters — read fields from the current element of the iterator.
 * Paths are dot-notation relative to the element object. */
CUTILS_MUST_USE
int  json_iter_get_str (const cutils_json_iter_t *iter, const char *path, char **out);
CUTILS_MUST_USE
int  json_iter_get_u32 (const cutils_json_iter_t *iter, const char *path,
                        uint32_t *out, uint32_t min, uint32_t max);
CUTILS_MUST_USE
int  json_iter_get_u64 (const cutils_json_iter_t *iter, const char *path,
                        uint64_t *out, uint64_t min, uint64_t max);
CUTILS_MUST_USE
int  json_iter_get_i32 (const cutils_json_iter_t *iter, const char *path,
                        int32_t *out, int32_t min, int32_t max);
CUTILS_MUST_USE
int  json_iter_get_i64 (const cutils_json_iter_t *iter, const char *path,
                        int64_t *out, int64_t min, int64_t max);
CUTILS_MUST_USE
int  json_iter_get_f64 (const cutils_json_iter_t *iter, const char *path,
                        double *out, double min, double max);
CUTILS_MUST_USE
int  json_iter_get_bool(const cutils_json_iter_t *iter, const char *path, bool *out);

/* ============================================================ */
/* Response side — build and serialize                          */
/* ============================================================ */

/* Allocate an empty response builder (root is an empty object). */
CUTILS_MUST_USE
int  json_resp_new     (cutils_json_resp_t **out);

/* Free a response builder. Safe to call with NULL. */
void json_resp_free    (cutils_json_resp_t *resp);

/* Serialize the response to a malloc'd buffer (caller frees).
 * Sets *len_out to the byte length (not including the terminating NUL).
 * The builder itself is unchanged and may be further modified or freed. */
CUTILS_MUST_USE
int  json_resp_finalize(cutils_json_resp_t *resp, char **buf_out, size_t *len_out);

/* --- Scalar adders ---
 *
 * Paths are dot-notation. Intermediate objects are created on demand.
 * All strings are copied; caller may free the source string immediately.
 * Adding at an existing path overwrites the previous value.
 */
CUTILS_MUST_USE int  json_resp_add_str (cutils_json_resp_t *resp, const char *path, const char *val);
CUTILS_MUST_USE int  json_resp_add_u32 (cutils_json_resp_t *resp, const char *path, uint32_t val);
CUTILS_MUST_USE int  json_resp_add_u64 (cutils_json_resp_t *resp, const char *path, uint64_t val);
CUTILS_MUST_USE int  json_resp_add_i32 (cutils_json_resp_t *resp, const char *path, int32_t val);
CUTILS_MUST_USE int  json_resp_add_i64 (cutils_json_resp_t *resp, const char *path, int64_t val);
CUTILS_MUST_USE int  json_resp_add_f64 (cutils_json_resp_t *resp, const char *path, double val);
CUTILS_MUST_USE int  json_resp_add_bool(cutils_json_resp_t *resp, const char *path, bool val);
CUTILS_MUST_USE int  json_resp_add_null(cutils_json_resp_t *resp, const char *path);

/* --- Array building ---
 *
 * For scalar arrays, use json_resp_array_append_*.
 * For arrays of objects, use the element builder pattern below:
 *
 *   for (size_t i = 0; i < n; i++) {
 *       cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
 *       if (json_resp_array_append_begin(resp, "ups_list", &elem) != CUTILS_OK)
 *           return -1;
 *       json_elem_add_str(&elem, "name", ups[i].name);
 *       json_elem_add_u32(&elem, "runtime_s", ups[i].runtime);
 *       / * cleanup attr commits elem into the array on scope exit * /
 *   }
 *
 * If a path doesn't yet exist as an array, the first append creates it.
 */

/* Append a scalar value to the array at path. Creates the array if absent. */
CUTILS_MUST_USE int  json_resp_array_append_str (cutils_json_resp_t *resp, const char *path, const char *val);
CUTILS_MUST_USE int  json_resp_array_append_u32 (cutils_json_resp_t *resp, const char *path, uint32_t val);
CUTILS_MUST_USE int  json_resp_array_append_u64 (cutils_json_resp_t *resp, const char *path, uint64_t val);
CUTILS_MUST_USE int  json_resp_array_append_i32 (cutils_json_resp_t *resp, const char *path, int32_t val);
CUTILS_MUST_USE int  json_resp_array_append_i64 (cutils_json_resp_t *resp, const char *path, int64_t val);
CUTILS_MUST_USE int  json_resp_array_append_f64 (cutils_json_resp_t *resp, const char *path, double val);
CUTILS_MUST_USE int  json_resp_array_append_bool(cutils_json_resp_t *resp, const char *path, bool val);
CUTILS_MUST_USE int  json_resp_array_append_null(cutils_json_resp_t *resp, const char *path);

/* Element builder handle — stack-allocated. */
typedef struct {
    /* Private — do not read or modify these fields. */
    void *_resp;
    void *_array;
    void *_obj;
    int   _committed;
} cutils_json_elem_t;

/* Begin building an object to append to the array at path.
 * The element must be explicitly committed via json_elem_commit() on the
 * success path. If the element leaves scope without being committed, the
 * cleanup helper json_elem_commit_p discards it — this prevents partial
 * elements from leaking into the response when an error path bails out. */
CUTILS_MUST_USE
int  json_resp_array_append_begin(cutils_json_resp_t *resp, const char *path,
                                  cutils_json_elem_t *elem);

/* Commit the element into its parent array. Safe to call multiple times
 * (subsequent calls are no-ops). Must be called on the success path;
 * otherwise the cleanup helper discards the element. */
void json_elem_commit(cutils_json_elem_t *elem);

/* Element scalar adders — paths are dot-notation relative to the element. */
CUTILS_MUST_USE int  json_elem_add_str (cutils_json_elem_t *elem, const char *path, const char *val);
CUTILS_MUST_USE int  json_elem_add_u32 (cutils_json_elem_t *elem, const char *path, uint32_t val);
CUTILS_MUST_USE int  json_elem_add_u64 (cutils_json_elem_t *elem, const char *path, uint64_t val);
CUTILS_MUST_USE int  json_elem_add_i32 (cutils_json_elem_t *elem, const char *path, int32_t val);
CUTILS_MUST_USE int  json_elem_add_i64 (cutils_json_elem_t *elem, const char *path, int64_t val);
CUTILS_MUST_USE int  json_elem_add_f64 (cutils_json_elem_t *elem, const char *path, double val);
CUTILS_MUST_USE int  json_elem_add_bool(cutils_json_elem_t *elem, const char *path, bool val);
CUTILS_MUST_USE int  json_elem_add_null(cutils_json_elem_t *elem, const char *path);

/* ============================================================ */
/* Cleanup helpers — for use with __attribute__((cleanup(...)))  */
/* ============================================================ */

void json_req_free_p   (cutils_json_req_t  **p);
void json_resp_free_p  (cutils_json_resp_t **p);
void json_iter_end_p   (cutils_json_iter_t  *p);

/* On scope exit: if json_elem_commit() was not called, the in-progress
 * element is discarded (freed, not appended). This is the intended
 * cleanup for json_resp_array_append_begin — call json_elem_commit()
 * explicitly on the success path. */
void json_elem_commit_p(cutils_json_elem_t *p);

/* Scoped cleanup macros — declare handles with these attributes to get
 * automatic cleanup on scope exit:
 *
 *   CUTILS_AUTO_JSON_REQ  cutils_json_req_t  *req  = NULL;
 *   CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
 *   CUTILS_AUTO_JSON_ITER cutils_json_iter_t  it;    / * stack object * /
 *   CUTILS_AUTO_JSON_ELEM cutils_json_elem_t  elem;  / * stack object * /
 *
 * Pointer-typed handles (REQ, RESP) must be initialized to NULL.
 * Stack-typed handles (ITER, ELEM) are initialized via the begin
 * functions — the cleanup helpers are no-ops on uninitialized state.
 * ELEM commits on json_elem_commit(), discards otherwise. */
#define CUTILS_AUTO_JSON_REQ  __attribute__((cleanup(json_req_free_p)))
#define CUTILS_AUTO_JSON_RESP __attribute__((cleanup(json_resp_free_p)))
#define CUTILS_AUTO_JSON_ITER __attribute__((cleanup(json_iter_end_p)))
#define CUTILS_AUTO_JSON_ELEM __attribute__((cleanup(json_elem_commit_p)))

#endif
