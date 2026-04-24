#include "cutils/json.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* File-local scoped cleanup for cJSON trees. */
static void cjson_delete_p(cJSON **p)
{
    if (*p) {
        cJSON_Delete(*p);
        *p = NULL;
    }
}
#define CUTILS_AUTO_CJSON __attribute__((cleanup(cjson_delete_p)))

/* ============================================================ */
/* Internal types                                                */
/* ============================================================ */

struct cutils_json_req {
    cJSON *root;
};

struct cutils_json_resp {
    cJSON *root;
};

/* Longest dot-separated segment accepted by the path walker.
 * Generous for realistic API field names; paths with longer segments
 * are rejected as invalid. */
#define PATH_SEG_MAX 128

/* ============================================================ */
/* Internal helpers                                              */
/* ============================================================ */

static const char *cjson_type_name(const cJSON *item)
{
    if (!item)                  return "missing";
    if (cJSON_IsNull(item))     return "null";
    if (cJSON_IsBool(item))     return "bool";
    if (cJSON_IsNumber(item))   return "number";
    if (cJSON_IsString(item))   return "string";
    if (cJSON_IsArray(item))    return "array";
    if (cJSON_IsObject(item))   return "object";
    return "unknown";
}

/* Walk a dot-notation path for reading. Returns the cJSON node at path,
 * or NULL if any segment is missing or an intermediate node is not an
 * object. Does not touch the error buffer — callers decide whether
 * absence is an error for their context. */
static cJSON *path_walk(cJSON *root, const char *path)
{
    if (!root || !path) return NULL;
    if (*path == '\0') return root;

    cJSON *cur = root;
    const char *p = path;
    char seg[PATH_SEG_MAX];

    while (*p) {
        size_t n = 0;
        while (*p && *p != '.') {
            if (n >= sizeof(seg) - 1) return NULL;
            seg[n++] = *p++;
        }
        seg[n] = '\0';
        if (n == 0) return NULL;            /* empty segment: leading/trailing/double dot */
        if (*p == '.') p++;
        if (!cJSON_IsObject(cur)) return NULL;
        cur = cJSON_GetObjectItemCaseSensitive(cur, seg);
        if (!cur) return NULL;
    }
    return cur;
}

/* Walk a dot-notation path for writing. On success, *parent_out is the
 * cJSON object that will receive the leaf, and *leaf_out is the final
 * segment (stored in leaf_buf). Creates intermediate objects as needed.
 * Fails if an intermediate exists as a non-object. */
static int path_walk_for_write(cJSON *root, const char *path,
                               cJSON **parent_out, const char **leaf_out,
                               char *leaf_buf, size_t leaf_buf_sz)
{
    if (!root || !path || *path == '\0') {
        return set_error(CUTILS_ERR_INVALID, "empty or null path");
    }

    const char *last_dot = strrchr(path, '.');
    const char *leaf_start = last_dot ? last_dot + 1 : path;
    size_t leaf_len = strlen(leaf_start);

    if (leaf_len == 0) {
        return set_error(CUTILS_ERR_INVALID, "path '%s' ends with '.'", path);
    }
    if (leaf_len >= leaf_buf_sz) {
        return set_error(CUTILS_ERR_INVALID, "path '%s' leaf too long", path);
    }
    memcpy(leaf_buf, leaf_start, leaf_len + 1);
    *leaf_out = leaf_buf;

    if (!last_dot) {
        *parent_out = root;
        return CUTILS_OK;
    }

    cJSON *cur = root;
    const char *p = path;
    char seg[PATH_SEG_MAX];

    while (p < last_dot) {
        size_t n = 0;
        while (p < last_dot && *p != '.') {
            if (n >= sizeof(seg) - 1) {
                return set_error(CUTILS_ERR_INVALID, "path '%s' segment too long", path);
            }
            seg[n++] = *p++;
        }
        seg[n] = '\0';
        if (n == 0) {
            return set_error(CUTILS_ERR_INVALID, "path '%s' has empty segment", path);
        }
        if (p < last_dot && *p == '.') p++;

        if (!cJSON_IsObject(cur)) {
            return set_error(CUTILS_ERR_JSON,
                             "path '%s': intermediate is not an object", path);
        }

        cJSON *next = cJSON_GetObjectItemCaseSensitive(cur, seg);
        if (!next) {
            next = cJSON_CreateObject();
            if (!next) {
                return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
            }
            if (!cJSON_AddItemToObject(cur, seg, next)) {
                cJSON_Delete(next);
                return set_error(CUTILS_ERR_JSON,
                                 "failed to attach object at '%s'", path);
            }
        } else if (!cJSON_IsObject(next)) {
            return set_error(CUTILS_ERR_JSON,
                             "path '%s': existing non-object at '%s'", path, seg);
        }
        cur = next;
    }

    *parent_out = cur;
    return CUTILS_OK;
}

/* ============================================================ */
/* Internal value extraction (shared by req + iter getters)     */
/* ============================================================ */

static int extract_str(cJSON *parent, const char *path, bool required,
                       char **out)
{
    *out = NULL;
    cJSON *item = path_walk(parent, path);
    if (!item) {
        if (!required) return CUTILS_OK;
        return set_error(CUTILS_ERR_JSON, "field '%s' missing", path);
    }
    if (!cJSON_IsString(item)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': expected string, got %s",
                         path, cjson_type_name(item));
    }
    const char *src = cJSON_GetStringValue(item);
    if (!src) src = "";
    char *dup = strdup(src);
    if (!dup) return set_error(CUTILS_ERR_NOMEM, "strdup failed for '%s'", path);
    *out = dup;
    return CUTILS_OK;
}

static int extract_number(cJSON *parent, const char *path, double *out)
{
    cJSON *item = path_walk(parent, path);
    if (!item) {
        return set_error(CUTILS_ERR_JSON, "field '%s' missing", path);
    }
    if (!cJSON_IsNumber(item)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': expected number, got %s",
                         path, cjson_type_name(item));
    }
    double d = cJSON_GetNumberValue(item);
    if (!isfinite(d)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': non-finite number", path);
    }
    *out = d;
    return CUTILS_OK;
}

static int extract_integer(cJSON *parent, const char *path, double *out)
{
    int rv = extract_number(parent, path, out);
    if (rv != CUTILS_OK) return rv;
    if (*out != floor(*out)) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': expected integer, got %g", path, *out);
    }
    return CUTILS_OK;
}

static int extract_bool(cJSON *parent, const char *path, bool *out)
{
    cJSON *item = path_walk(parent, path);
    if (!item) {
        return set_error(CUTILS_ERR_JSON, "field '%s' missing", path);
    }
    if (!cJSON_IsBool(item)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': expected bool, got %s",
                         path, cjson_type_name(item));
    }
    *out = cJSON_IsTrue(item) ? true : false;
    return CUTILS_OK;
}

/* Narrowing checks for integer getters. Each verifies the double is in
 * range for the target type and, for signed narrowings, that sign matches.
 * cJSON stores all numbers as double, so we validate numerically. */
static int narrow_u32(const char *path, double d, uint32_t min, uint32_t max,
                      uint32_t *out)
{
    if (d < 0.0 || d > (double)UINT32_MAX) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of u32 range: %g", path, d);
    }
    uint32_t v = (uint32_t)d;
    if (v < min || v > max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of range [%u,%u]: %u",
                         path, min, max, v);
    }
    *out = v;
    return CUTILS_OK;
}

static int narrow_u64(const char *path, double d, uint64_t min, uint64_t max,
                      uint64_t *out)
{
    /* Double can represent up to 2^53 exactly; above that, precision loss.
     * Cap at 2^53 to ensure the integer we return is what was parsed. */
    const double u64_max_exact = 9007199254740992.0; /* 2^53 */
    if (d < 0.0 || d > u64_max_exact) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of representable u64 range: %g", path, d);
    }
    uint64_t v = (uint64_t)d;
    if (v < min || v > max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of range [%llu,%llu]: %llu",
                         path, (unsigned long long)min,
                         (unsigned long long)max, (unsigned long long)v);
    }
    *out = v;
    return CUTILS_OK;
}

static int narrow_i32(const char *path, double d, int32_t min, int32_t max,
                      int32_t *out)
{
    if (d < (double)INT32_MIN || d > (double)INT32_MAX) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of i32 range: %g", path, d);
    }
    int32_t v = (int32_t)d;
    if (v < min || v > max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of range [%d,%d]: %d",
                         path, min, max, v);
    }
    *out = v;
    return CUTILS_OK;
}

static int narrow_i64(const char *path, double d, int64_t min, int64_t max,
                      int64_t *out)
{
    const double i64_min_exact = -9007199254740992.0; /* -2^53 */
    const double i64_max_exact =  9007199254740992.0; /*  2^53 */
    if (d < i64_min_exact || d > i64_max_exact) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of representable i64 range: %g", path, d);
    }
    int64_t v = (int64_t)d;
    if (v < min || v > max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of range [%lld,%lld]: %lld",
                         path, (long long)min, (long long)max, (long long)v);
    }
    *out = v;
    return CUTILS_OK;
}

static int narrow_f64(const char *path, double d, double min, double max,
                      double *out)
{
    if (d < min || d > max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': out of range [%g,%g]: %g",
                         path, min, max, d);
    }
    *out = d;
    return CUTILS_OK;
}

/* ============================================================ */
/* Internal response setters                                     */
/* ============================================================ */

/* Attach item to the object at path, replacing any existing entry at the
 * leaf key. Takes ownership of item: frees it on any error, adopts it
 * on success. */
static int set_at_path(cJSON *root, const char *path, cJSON *item)
{
    cJSON *parent = NULL;
    const char *leaf = NULL;
    char leaf_buf[PATH_SEG_MAX];

    int rv = path_walk_for_write(root, path, &parent, &leaf, leaf_buf, sizeof(leaf_buf));
    if (rv != CUTILS_OK) {
        cJSON_Delete(item);
        return rv;
    }
    if (!cJSON_IsObject(parent)) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_JSON, "path '%s': parent is not an object", path);
    }

    if (cJSON_GetObjectItemCaseSensitive(parent, leaf)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, leaf);
    }
    if (!cJSON_AddItemToObject(parent, leaf, item)) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_JSON, "failed to attach at path '%s'", path);
    }
    return CUTILS_OK;
}

/* Ensure an array exists at path (creating if needed). On success,
 * *array_out points to the (possibly new) array. */
static int ensure_array_at(cJSON *root, const char *path, cJSON **array_out)
{
    cJSON *existing = path_walk(root, path);
    if (existing) {
        if (!cJSON_IsArray(existing)) {
            return set_error(CUTILS_ERR_JSON,
                             "path '%s' exists but is not an array", path);
        }
        *array_out = existing;
        return CUTILS_OK;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    int rv = set_at_path(root, path, arr); /* takes ownership */
    if (rv != CUTILS_OK) return rv;
    *array_out = arr;
    return CUTILS_OK;
}

/* Append item to the array at path (creating the array if needed).
 * Takes ownership of item. */
static int append_at_path(cJSON *root, const char *path, cJSON *item)
{
    cJSON *arr = NULL;
    int rv = ensure_array_at(root, path, &arr);
    if (rv != CUTILS_OK) {
        cJSON_Delete(item);
        return rv;
    }
    if (!cJSON_AddItemToArray(arr, item)) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_JSON, "failed to append to '%s'", path);
    }
    return CUTILS_OK;
}

/* ============================================================ */
/* Request — lifecycle                                           */
/* ============================================================ */

int json_req_parse(const char *buf, size_t len, cutils_json_req_t **out)
{
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    *out = NULL;
    if (!buf) return set_error(CUTILS_ERR_INVALID, "null buf");

    CUTILS_AUTO_CJSON cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        const char *errp = cJSON_GetErrorPtr();
        if (errp && *errp)
            return set_error(CUTILS_ERR_JSON,
                             "JSON parse failed near: %.32s", errp);
        return set_error(CUTILS_ERR_JSON, "JSON parse failed");
    }
    if (!cJSON_IsObject(root))
        return set_error(CUTILS_ERR_JSON, "JSON root is not an object");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *req = calloc(1, sizeof(*req));
    if (!req) return set_error(CUTILS_ERR_NOMEM, "req alloc failed");

    req->root = CUTILS_MOVE(root);    /* req owns root now */
    *out      = CUTILS_MOVE(req);     /* caller owns req now */
    return CUTILS_OK;
}

void json_req_free(cutils_json_req_t *req)
{
    if (!req) return;
    if (req->root) cJSON_Delete(req->root);
    free(req);
}

void json_req_free_p(cutils_json_req_t **p)
{
    if (p && *p) {
        json_req_free(*p);
        *p = NULL;
    }
}

/* ============================================================ */
/* Request — scalar getters                                      */
/* ============================================================ */

int json_req_get_str(const cutils_json_req_t *req, const char *path, char **out)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    return extract_str(req->root, path, true, out);
}

int json_req_get_str_opt(const cutils_json_req_t *req, const char *path, char **out)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    return extract_str(req->root, path, false, out);
}

int json_req_get_u32(const cutils_json_req_t *req, const char *path,
                     uint32_t *out, uint32_t min, uint32_t max)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    double d;
    int rv = extract_integer(req->root, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_u32(path, d, min, max, out);
}

int json_req_get_u64(const cutils_json_req_t *req, const char *path,
                     uint64_t *out, uint64_t min, uint64_t max)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    double d;
    int rv = extract_integer(req->root, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_u64(path, d, min, max, out);
}

int json_req_get_i32(const cutils_json_req_t *req, const char *path,
                     int32_t *out, int32_t min, int32_t max)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    double d;
    int rv = extract_integer(req->root, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_i32(path, d, min, max, out);
}

int json_req_get_i64(const cutils_json_req_t *req, const char *path,
                     int64_t *out, int64_t min, int64_t max)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    double d;
    int rv = extract_integer(req->root, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_i64(path, d, min, max, out);
}

int json_req_get_f64(const cutils_json_req_t *req, const char *path,
                     double *out, double min, double max)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    double d;
    int rv = extract_number(req->root, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_f64(path, d, min, max, out);
}

int json_req_get_bool(const cutils_json_req_t *req, const char *path, bool *out)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    return extract_bool(req->root, path, out);
}

/* ============================================================ */
/* Request — presence and structure                              */
/* ============================================================ */

bool json_req_has(const cutils_json_req_t *req, const char *path)
{
    if (!req) return false;
    return path_walk(req->root, path) != NULL;
}

bool json_req_is_null(const cutils_json_req_t *req, const char *path)
{
    if (!req) return false;
    cJSON *item = path_walk(req->root, path);
    return item != NULL && cJSON_IsNull(item);
}

int json_req_array_len(const cutils_json_req_t *req, const char *path, size_t *out)
{
    if (!req || !out) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = path_walk(req->root, path);
    if (!item) return set_error(CUTILS_ERR_JSON, "field '%s' missing", path);
    if (!cJSON_IsArray(item)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': expected array, got %s",
                         path, cjson_type_name(item));
    }
    int sz = cJSON_GetArraySize(item);
    *out = sz < 0 ? 0U : (size_t)sz;
    return CUTILS_OK;
}

/* ============================================================ */
/* Iterator                                                      */
/* ============================================================ */

int json_iter_begin(const cutils_json_req_t *req, const char *path,
                    cutils_json_iter_t *iter)
{
    if (!iter) return set_error(CUTILS_ERR_INVALID, "null iter");
    memset(iter, 0, sizeof(*iter));
    if (!req) return set_error(CUTILS_ERR_INVALID, "null req");

    cJSON *arr = path_walk(req->root, path);
    if (!arr) return set_error(CUTILS_ERR_JSON, "field '%s' missing", path);
    if (!cJSON_IsArray(arr)) {
        return set_error(CUTILS_ERR_JSON, "field '%s': expected array, got %s",
                         path, cjson_type_name(arr));
    }
    int sz = cJSON_GetArraySize(arr);
    iter->_array = arr;
    iter->_current = NULL;
    iter->_index = 0;
    iter->_len = sz < 0 ? 0U : (size_t)sz;
    return CUTILS_OK;
}

bool json_iter_next(cutils_json_iter_t *iter)
{
    if (!iter || !iter->_array) return false;
    if (iter->_index >= iter->_len) {
        iter->_current = NULL;
        return false;
    }
    /* cJSON_GetArrayItem takes int; indices within our captured _len
     * were produced by cJSON_GetArraySize which returns int, so the
     * cast is safe. */
    iter->_current = cJSON_GetArrayItem((cJSON *)iter->_array, (int)iter->_index);
    iter->_index++;
    return iter->_current != NULL;
}

void json_iter_end(cutils_json_iter_t *iter)
{
    /* Iterator holds only borrowed pointers into the request tree —
     * nothing to free. Zero the handle so stale use is easier to spot. */
    if (iter) memset(iter, 0, sizeof(*iter));
}

void json_iter_end_p(cutils_json_iter_t *p)
{
    json_iter_end(p);
}

static int iter_check(const cutils_json_iter_t *iter)
{
    if (!iter) return set_error(CUTILS_ERR_INVALID, "null iter");
    if (!iter->_current) {
        return set_error(CUTILS_ERR_INVALID,
                         "iterator not positioned (call json_iter_next first)");
    }
    return CUTILS_OK;
}

int json_iter_get_str(const cutils_json_iter_t *iter, const char *path, char **out)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    return extract_str(iter->_current, path, true, out);
}

int json_iter_get_u32(const cutils_json_iter_t *iter, const char *path,
                      uint32_t *out, uint32_t min, uint32_t max)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    double d;
    rv = extract_integer(iter->_current, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_u32(path, d, min, max, out);
}

int json_iter_get_u64(const cutils_json_iter_t *iter, const char *path,
                      uint64_t *out, uint64_t min, uint64_t max)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    double d;
    rv = extract_integer(iter->_current, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_u64(path, d, min, max, out);
}

int json_iter_get_i32(const cutils_json_iter_t *iter, const char *path,
                      int32_t *out, int32_t min, int32_t max)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    double d;
    rv = extract_integer(iter->_current, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_i32(path, d, min, max, out);
}

int json_iter_get_i64(const cutils_json_iter_t *iter, const char *path,
                      int64_t *out, int64_t min, int64_t max)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    double d;
    rv = extract_integer(iter->_current, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_i64(path, d, min, max, out);
}

int json_iter_get_f64(const cutils_json_iter_t *iter, const char *path,
                      double *out, double min, double max)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    double d;
    rv = extract_number(iter->_current, path, &d);
    if (rv != CUTILS_OK) return rv;
    return narrow_f64(path, d, min, max, out);
}

int json_iter_get_bool(const cutils_json_iter_t *iter, const char *path, bool *out)
{
    int rv = iter_check(iter); if (rv != CUTILS_OK) return rv;
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    return extract_bool(iter->_current, path, out);
}

/* ============================================================ */
/* Response — lifecycle                                          */
/* ============================================================ */

int json_resp_new(cutils_json_resp_t **out)
{
    if (!out) return set_error(CUTILS_ERR_INVALID, "null out");
    *out = NULL;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *r = calloc(1, sizeof(*r));
    if (!r) return set_error(CUTILS_ERR_NOMEM, "resp alloc failed");
    r->root = cJSON_CreateObject();
    /* CUTILS_AUTO_JSON_RESP frees r on any return. */
    /* cppcheck-suppress memleak */
    if (!r->root) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    *out = CUTILS_MOVE(r);
    return CUTILS_OK;
}

void json_resp_free(cutils_json_resp_t *resp)
{
    if (!resp) return;
    if (resp->root) cJSON_Delete(resp->root);
    free(resp);
}

void json_resp_free_p(cutils_json_resp_t **p)
{
    if (p && *p) {
        json_resp_free(*p);
        *p = NULL;
    }
}

int json_resp_finalize(cutils_json_resp_t *resp, char **buf_out, size_t *len_out)
{
    if (!resp || !buf_out) return set_error(CUTILS_ERR_INVALID, "null arg");
    *buf_out = NULL;
    if (len_out) *len_out = 0;

    char *s = cJSON_PrintUnformatted(resp->root);
    if (!s) return set_error(CUTILS_ERR_NOMEM, "cJSON print failed");
    *buf_out = s;
    if (len_out) *len_out = strlen(s);
    return CUTILS_OK;
}

/* ============================================================ */
/* Response — scalar adders (top-level)                          */
/* ============================================================ */

int json_resp_add_str(cutils_json_resp_t *resp, const char *path, const char *val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateString(val ? val : "");
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_u32(cutils_json_resp_t *resp, const char *path, uint32_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_u64(cutils_json_resp_t *resp, const char *path, uint64_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    const uint64_t exact_max = 9007199254740992ULL; /* 2^53 */
    if (val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': u64 value %llu exceeds JSON integer precision (2^53)",
                         path, (unsigned long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_i32(cutils_json_resp_t *resp, const char *path, int32_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_i64(cutils_json_resp_t *resp, const char *path, int64_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    const int64_t exact_min = -9007199254740992LL;
    const int64_t exact_max =  9007199254740992LL;
    if (val < exact_min || val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': i64 value %lld exceeds JSON integer precision (±2^53)",
                         path, (long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_f64(cutils_json_resp_t *resp, const char *path, double val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    if (!isfinite(val)) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': non-finite number cannot be encoded as JSON", path);
    }
    cJSON *item = cJSON_CreateNumber(val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_bool(cutils_json_resp_t *resp, const char *path, bool val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateBool(val ? 1 : 0);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

int json_resp_add_null(cutils_json_resp_t *resp, const char *path)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNull();
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return set_at_path(resp->root, path, item);
}

/* ============================================================ */
/* Response — array scalar append                                */
/* ============================================================ */

int json_resp_array_append_str(cutils_json_resp_t *resp, const char *path, const char *val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateString(val ? val : "");
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_u32(cutils_json_resp_t *resp, const char *path, uint32_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_u64(cutils_json_resp_t *resp, const char *path, uint64_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    const uint64_t exact_max = 9007199254740992ULL;
    if (val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': u64 value %llu exceeds JSON integer precision (2^53)",
                         path, (unsigned long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_i32(cutils_json_resp_t *resp, const char *path, int32_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_i64(cutils_json_resp_t *resp, const char *path, int64_t val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    const int64_t exact_min = -9007199254740992LL;
    const int64_t exact_max =  9007199254740992LL;
    if (val < exact_min || val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': i64 value %lld exceeds JSON integer precision (±2^53)",
                         path, (long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_f64(cutils_json_resp_t *resp, const char *path, double val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    if (!isfinite(val)) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': non-finite number cannot be encoded as JSON", path);
    }
    cJSON *item = cJSON_CreateNumber(val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_bool(cutils_json_resp_t *resp, const char *path, bool val)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateBool(val ? 1 : 0);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

int json_resp_array_append_null(cutils_json_resp_t *resp, const char *path)
{
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");
    cJSON *item = cJSON_CreateNull();
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return append_at_path(resp->root, path, item);
}

/* ============================================================ */
/* Response — element builder (array of objects)                 */
/* ============================================================ */

int json_resp_array_append_begin(cutils_json_resp_t *resp, const char *path,
                                 cutils_json_elem_t *elem)
{
    if (!elem) return set_error(CUTILS_ERR_INVALID, "null elem");
    memset(elem, 0, sizeof(*elem));
    if (!resp || !path) return set_error(CUTILS_ERR_INVALID, "null arg");

    cJSON *arr = NULL;
    int rv = ensure_array_at(resp->root, path, &arr);
    if (rv != CUTILS_OK) return rv;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");

    elem->_resp      = resp;
    elem->_array     = arr;
    elem->_obj       = obj;
    elem->_committed = 0;
    return CUTILS_OK;
}

void json_elem_commit(cutils_json_elem_t *elem)
{
    if (!elem || elem->_committed) return;
    if (elem->_array && elem->_obj) {
        /* cJSON_AddItemToArray can technically fail on alloc, but the
         * detached object is already fully built — if attachment fails
         * we free it rather than leak. */
        if (!cJSON_AddItemToArray((cJSON *)elem->_array, (cJSON *)elem->_obj)) {
            cJSON_Delete((cJSON *)elem->_obj);
        }
        elem->_obj = NULL;
    }
    elem->_committed = 1;
}

void json_elem_commit_p(cutils_json_elem_t *p)
{
    /* Cleanup path: if the caller never committed, discard the partial
     * object. This prevents half-built elements from leaking into the
     * response when an error path short-circuits the build. */
    if (!p) return;
    if (!p->_committed) {
        if (p->_obj) {
            cJSON_Delete((cJSON *)p->_obj);
            p->_obj = NULL;
        }
        p->_committed = 1;
    }
}

/* Internal helper — add item to the element's in-progress object. */
static int elem_set(cutils_json_elem_t *elem, const char *path, cJSON *item)
{
    if (!elem || !path) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_INVALID, "null arg");
    }
    if (elem->_committed) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_INVALID,
                         "element already committed; cannot modify");
    }
    if (!elem->_obj) {
        cJSON_Delete(item);
        return set_error(CUTILS_ERR_INVALID, "element not initialized");
    }
    return set_at_path((cJSON *)elem->_obj, path, item);
}

int json_elem_add_str(cutils_json_elem_t *elem, const char *path, const char *val)
{
    cJSON *item = cJSON_CreateString(val ? val : "");
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_u32(cutils_json_elem_t *elem, const char *path, uint32_t val)
{
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_u64(cutils_json_elem_t *elem, const char *path, uint64_t val)
{
    const uint64_t exact_max = 9007199254740992ULL;
    if (val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': u64 value %llu exceeds JSON integer precision (2^53)",
                         path ? path : "", (unsigned long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_i32(cutils_json_elem_t *elem, const char *path, int32_t val)
{
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_i64(cutils_json_elem_t *elem, const char *path, int64_t val)
{
    const int64_t exact_min = -9007199254740992LL;
    const int64_t exact_max =  9007199254740992LL;
    if (val < exact_min || val > exact_max) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': i64 value %lld exceeds JSON integer precision (±2^53)",
                         path ? path : "", (long long)val);
    }
    cJSON *item = cJSON_CreateNumber((double)val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_f64(cutils_json_elem_t *elem, const char *path, double val)
{
    if (!isfinite(val)) {
        return set_error(CUTILS_ERR_JSON,
                         "field '%s': non-finite number cannot be encoded as JSON",
                         path ? path : "");
    }
    cJSON *item = cJSON_CreateNumber(val);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_bool(cutils_json_elem_t *elem, const char *path, bool val)
{
    cJSON *item = cJSON_CreateBool(val ? 1 : 0);
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}

int json_elem_add_null(cutils_json_elem_t *elem, const char *path)
{
    cJSON *item = cJSON_CreateNull();
    if (!item) return set_error(CUTILS_ERR_NOMEM, "cJSON alloc failed");
    return elem_set(elem, path, item);
}
