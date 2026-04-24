#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cutils/error.h"
#include "cutils/json.h"
#include "cutils/mem.h"

static int setup_clear_error(void **state)
{
    (void)state;
    cutils_clear_error();
    return 0;
}

/* Small helper to parse a literal for readability. */
static cutils_json_req_t *parse_or_fail(const char *s)
{
    cutils_json_req_t *req = NULL;
    int rv = json_req_parse(s, strlen(s), &req);
    assert_int_equal(rv, CUTILS_OK);
    assert_non_null(req);
    return req;
}

/* ============================================================ */
/* Parse / free                                                  */
/* ============================================================ */

static void test_parse_simple_object(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"a\":1}");
    json_req_free(req);
}

static void test_parse_null_buf(void **state)
{
    (void)state;
    cutils_json_req_t *req = NULL;
    int rv = json_req_parse(NULL, 0, &req);
    assert_int_equal(rv, CUTILS_ERR_INVALID);
    assert_null(req);
}

static void test_parse_null_out(void **state)
{
    (void)state;
    int rv = json_req_parse("{}", 2, NULL);
    assert_int_equal(rv, CUTILS_ERR_INVALID);
}

static void test_parse_malformed(void **state)
{
    (void)state;
    cutils_json_req_t *req = NULL;
    int rv = json_req_parse("{not json", 9, &req);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_null(req);
    assert_non_null(strstr(cutils_get_error(), "parse"));
}

static void test_parse_non_object(void **state)
{
    (void)state;
    cutils_json_req_t *req = NULL;
    int rv = json_req_parse("[1,2,3]", 7, &req);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_null(req);
    assert_non_null(strstr(cutils_get_error(), "not an object"));
}

static void test_free_null_safe(void **state)
{
    (void)state;
    json_req_free(NULL);                          /* must not crash */
    cutils_json_req_t *p = NULL;
    json_req_free_p(&p);                          /* NULL pointer */
    json_req_free_p(NULL);                        /* NULL address */
}

/* ============================================================ */
/* Request — string getters                                      */
/* ============================================================ */

static void test_get_str_happy(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"name\":\"bob\"}");
    char *out __attribute__((cleanup(cutils_free_p))) = NULL;
    int rv = json_req_get_str(req, "name", &out);
    assert_int_equal(rv, CUTILS_OK);
    assert_string_equal(out, "bob");
    json_req_free(req);
}

static void test_get_str_missing(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"other\":1}");
    char *out = NULL;
    int rv = json_req_get_str(req, "name", &out);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_null(out);
    assert_non_null(strstr(cutils_get_error(), "name"));
    assert_non_null(strstr(cutils_get_error(), "missing"));
    json_req_free(req);
}

static void test_get_str_wrong_type(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"name\":42}");
    char *out = NULL;
    int rv = json_req_get_str(req, "name", &out);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_null(out);
    assert_non_null(strstr(cutils_get_error(), "expected string"));
    json_req_free(req);
}

static void test_get_str_opt_missing_returns_null(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"other\":1}");
    char *out = (char *)0xDEAD; /* ensure it's overwritten */
    int rv = json_req_get_str_opt(req, "name", &out);
    assert_int_equal(rv, CUTILS_OK);
    assert_null(out);
    json_req_free(req);
}

static void test_get_str_opt_present_returns_copy(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"name\":\"alice\"}");
    char *out __attribute__((cleanup(cutils_free_p))) = NULL;
    int rv = json_req_get_str_opt(req, "name", &out);
    assert_int_equal(rv, CUTILS_OK);
    assert_string_equal(out, "alice");
    json_req_free(req);
}

static void test_get_str_opt_wrong_type_still_errors(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"name\":42}");
    char *out = NULL;
    int rv = json_req_get_str_opt(req, "name", &out);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

/* ============================================================ */
/* Request — numeric getters                                     */
/* ============================================================ */

static void test_get_u32_happy(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"port\":8080}");
    uint32_t port = 0;
    int rv = json_req_get_u32(req, "port", &port, 1, 65535);
    assert_int_equal(rv, CUTILS_OK);
    assert_int_equal(port, 8080);
    json_req_free(req);
}

static void test_get_u32_above_max(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"port\":99999}");
    uint32_t port = 0;
    int rv = json_req_get_u32(req, "port", &port, 1, 65535);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "out of range"));
    json_req_free(req);
}

static void test_get_u32_below_min(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"port\":0}");
    uint32_t port = 0;
    int rv = json_req_get_u32(req, "port", &port, 1, 65535);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_get_u32_rejects_negative(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"port\":-1}");
    uint32_t port = 0;
    int rv = json_req_get_u32(req, "port", &port, 0, UINT32_MAX);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_get_u32_rejects_non_integer(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"x\":1.5}");
    uint32_t x = 0;
    int rv = json_req_get_u32(req, "x", &x, 0, UINT32_MAX);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "integer"));
    json_req_free(req);
}

static void test_get_u32_wrong_type(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"port\":\"8080\"}");
    uint32_t port = 0;
    int rv = json_req_get_u32(req, "port", &port, 0, UINT32_MAX);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "expected number"));
    json_req_free(req);
}

static void test_get_u64_large_in_range(void **state)
{
    (void)state;
    /* 2^40 — well inside exact double range */
    cutils_json_req_t *req = parse_or_fail("{\"n\":1099511627776}");
    uint64_t n = 0;
    int rv = json_req_get_u64(req, "n", &n, 0, UINT64_MAX);
    assert_int_equal(rv, CUTILS_OK);
    assert_int_equal(n, 1099511627776ULL);
    json_req_free(req);
}

static void test_get_u64_exceeds_2to53(void **state)
{
    (void)state;
    /* 2^53 + 2 — exactly representable as a double (evens in [2^53,2^54]
     * are), so cJSON's parse preserves it; our check then rejects. Using
     * 2^53 + 1 would round down to 2^53 and look "in range" after parse. */
    cutils_json_req_t *req = parse_or_fail("{\"n\":9007199254740994}");
    uint64_t n = 0;
    int rv = json_req_get_u64(req, "n", &n, 0, UINT64_MAX);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "u64"));
    json_req_free(req);
}

static void test_get_i32_happy_negative(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"delta\":-42}");
    int32_t d = 0;
    int rv = json_req_get_i32(req, "delta", &d, INT32_MIN, INT32_MAX);
    assert_int_equal(rv, CUTILS_OK);
    assert_int_equal(d, -42);
    json_req_free(req);
}

static void test_get_i32_out_of_range(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"x\":10}");
    int32_t x = 0;
    int rv = json_req_get_i32(req, "x", &x, -5, 5);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_get_i64_exceeds_2to53(void **state)
{
    (void)state;
    /* See u64 test above for why 2^53+2 (not 2^53+1) — same reasoning,
     * mirrored on the negative side. */
    cutils_json_req_t *req = parse_or_fail("{\"n\":-9007199254740994}");
    int64_t n = 0;
    int rv = json_req_get_i64(req, "n", &n, INT64_MIN, INT64_MAX);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_get_f64_happy(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"temp\":23.5}");
    double t = 0.0;
    int rv = json_req_get_f64(req, "temp", &t, -100.0, 200.0);
    assert_int_equal(rv, CUTILS_OK);
    assert_true(t > 23.49 && t < 23.51);
    json_req_free(req);
}

static void test_get_f64_out_of_range(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"v\":5.0}");
    double v = 0.0;
    int rv = json_req_get_f64(req, "v", &v, 0.0, 1.0);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_get_bool_true(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"on\":true}");
    bool b = false;
    int rv = json_req_get_bool(req, "on", &b);
    assert_int_equal(rv, CUTILS_OK);
    assert_true(b);
    json_req_free(req);
}

static void test_get_bool_false(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"on\":false}");
    bool b = true;
    int rv = json_req_get_bool(req, "on", &b);
    assert_int_equal(rv, CUTILS_OK);
    assert_false(b);
    json_req_free(req);
}

static void test_get_bool_wrong_type(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"on\":1}");
    bool b = false;
    int rv = json_req_get_bool(req, "on", &b);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

/* ============================================================ */
/* Paths                                                         */
/* ============================================================ */

static void test_dotted_path_ok(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail(
        "{\"network\":{\"dns\":{\"primary\":\"1.1.1.1\"}}}");
    char *out __attribute__((cleanup(cutils_free_p))) = NULL;
    int rv = json_req_get_str(req, "network.dns.primary", &out);
    assert_int_equal(rv, CUTILS_OK);
    assert_string_equal(out, "1.1.1.1");
    json_req_free(req);
}

static void test_path_missing_intermediate(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"network\":{}}");
    char *out = NULL;
    int rv = json_req_get_str(req, "network.dns.primary", &out);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_path_non_object_intermediate(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"a\":5}");
    char *out = NULL;
    int rv = json_req_get_str(req, "a.b", &out);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_path_empty_segment_rejected(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"a\":1}");
    char *out = NULL;
    /* Leading dot, trailing dot, and double dot should all fail lookup. */
    assert_int_equal(json_req_get_str(req, ".a", &out), CUTILS_ERR_JSON);
    assert_int_equal(json_req_get_str(req, "a.", &out), CUTILS_ERR_JSON);
    assert_int_equal(json_req_get_str(req, "a..b", &out), CUTILS_ERR_JSON);
    json_req_free(req);
}

/* ============================================================ */
/* Presence / structure                                          */
/* ============================================================ */

static void test_has_present_and_missing(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"a\":1,\"b\":null}");
    assert_true(json_req_has(req, "a"));
    assert_true(json_req_has(req, "b"));        /* null still "has" */
    assert_false(json_req_has(req, "c"));
    assert_false(json_req_has(NULL, "a"));      /* NULL-safe */
    json_req_free(req);
}

static void test_is_null(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"a\":null,\"b\":1,\"c\":\"\"}");
    assert_true(json_req_is_null(req, "a"));
    assert_false(json_req_is_null(req, "b"));
    assert_false(json_req_is_null(req, "c"));   /* empty string is not null */
    assert_false(json_req_is_null(req, "missing"));
    json_req_free(req);
}

static void test_array_len(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"xs\":[1,2,3],\"empty\":[]}");
    size_t n = 0;
    assert_int_equal(json_req_array_len(req, "xs", &n), CUTILS_OK);
    assert_int_equal(n, 3);
    assert_int_equal(json_req_array_len(req, "empty", &n), CUTILS_OK);
    assert_int_equal(n, 0);
    assert_int_equal(json_req_array_len(req, "missing", &n), CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_array_len_non_array(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"x\":{}}");
    size_t n = 0;
    assert_int_equal(json_req_array_len(req, "x", &n), CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "expected array"));
    json_req_free(req);
}

/* ============================================================ */
/* Iterator                                                      */
/* ============================================================ */

static void test_iter_walks_array_of_objects(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail(
        "{\"ups_list\":["
        " {\"name\":\"a\",\"port\":1},"
        " {\"name\":\"b\",\"port\":2},"
        " {\"name\":\"c\",\"port\":3}"
        "]}");

    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "ups_list", &it), CUTILS_OK);

    int seen = 0;
    while (json_iter_next(&it)) {
        char *name __attribute__((cleanup(cutils_free_p))) = NULL;
        uint32_t port = 0;
        assert_int_equal(json_iter_get_str(&it, "name", &name), CUTILS_OK);
        assert_int_equal(json_iter_get_u32(&it, "port", &port, 1, 10), CUTILS_OK);
        assert_int_equal((int)port, seen + 1);
        assert_int_equal(strlen(name), 1);
        seen++;
    }
    assert_int_equal(seen, 3);
    json_req_free(req);
}

static void test_iter_empty_array(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"xs\":[]}");
    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "xs", &it), CUTILS_OK);
    assert_false(json_iter_next(&it));
    json_req_free(req);
}

static void test_iter_missing_path_errors(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{}");
    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "xs", &it), CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_iter_non_array_errors(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"x\":{}}");
    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "x", &it), CUTILS_ERR_JSON);
    json_req_free(req);
}

static void test_iter_get_before_next_errors(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail("{\"xs\":[{\"a\":1}]}");
    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "xs", &it), CUTILS_OK);
    char *out = NULL;
    /* Did not call iter_next; _current is NULL → error */
    assert_int_equal(json_iter_get_str(&it, "a", &out), CUTILS_ERR_INVALID);
    assert_null(out);
    json_req_free(req);
}

static void test_iter_get_all_types(void **state)
{
    (void)state;
    cutils_json_req_t *req = parse_or_fail(
        "{\"xs\":[{"
        " \"s\":\"hi\",\"u32\":5,\"u64\":7,"
        " \"i32\":-5,\"i64\":-7,\"f64\":1.5,\"b\":true"
        "}]}");

    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    assert_int_equal(json_iter_begin(req, "xs", &it), CUTILS_OK);
    assert_true(json_iter_next(&it));

    char *s __attribute__((cleanup(cutils_free_p))) = NULL;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    double f = 0.0;
    bool b = false;

    assert_int_equal(json_iter_get_str(&it, "s", &s), CUTILS_OK);
    assert_int_equal(json_iter_get_u32(&it, "u32", &u32, 0, UINT32_MAX), CUTILS_OK);
    assert_int_equal(json_iter_get_u64(&it, "u64", &u64, 0, UINT64_MAX), CUTILS_OK);
    assert_int_equal(json_iter_get_i32(&it, "i32", &i32, INT32_MIN, INT32_MAX), CUTILS_OK);
    assert_int_equal(json_iter_get_i64(&it, "i64", &i64, INT64_MIN, INT64_MAX), CUTILS_OK);
    assert_int_equal(json_iter_get_f64(&it, "f64", &f, -10.0, 10.0), CUTILS_OK);
    assert_int_equal(json_iter_get_bool(&it, "b", &b), CUTILS_OK);

    assert_string_equal(s, "hi");
    assert_int_equal(u32, 5);
    assert_int_equal(u64, 7);
    assert_int_equal(i32, -5);
    assert_int_equal(i64, -7);
    assert_true(f > 1.49 && f < 1.51);
    assert_true(b);

    json_req_free(req);
}

/* ============================================================ */
/* Response — lifecycle                                          */
/* ============================================================ */

static void test_resp_new_empty_finalizes_to_empty_object(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    assert_int_equal(json_resp_new(&resp), CUTILS_OK);

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    assert_int_equal(json_resp_finalize(resp, &buf, &len), CUTILS_OK);
    assert_string_equal(buf, "{}");
    assert_int_equal(len, 2);
}

static void test_resp_free_null_safe(void **state)
{
    (void)state;
    json_resp_free(NULL);
    cutils_json_resp_t *p = NULL;
    json_resp_free_p(&p);
    json_resp_free_p(NULL);
}

/* ============================================================ */
/* Response — scalar adders                                      */
/* ============================================================ */

static void test_resp_add_all_scalars(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    assert_int_equal(json_resp_new(&resp), CUTILS_OK);

    assert_int_equal(json_resp_add_str  (resp, "s",   "hello"),       CUTILS_OK);
    assert_int_equal(json_resp_add_u32  (resp, "u32", 100U),          CUTILS_OK);
    assert_int_equal(json_resp_add_u64  (resp, "u64", 200ULL),        CUTILS_OK);
    assert_int_equal(json_resp_add_i32  (resp, "i32", -300),          CUTILS_OK);
    assert_int_equal(json_resp_add_i64  (resp, "i64", -400LL),        CUTILS_OK);
    assert_int_equal(json_resp_add_f64  (resp, "f64", 2.5),           CUTILS_OK);
    assert_int_equal(json_resp_add_bool (resp, "b",   true),          CUTILS_OK);
    assert_int_equal(json_resp_add_null (resp, "n"),                  CUTILS_OK);

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    assert_int_equal(json_resp_finalize(resp, &buf, &len), CUTILS_OK);

    /* Round-trip verify every field */
    cutils_json_req_t *req = parse_or_fail(buf);

    char *s __attribute__((cleanup(cutils_free_p))) = NULL;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    double f = 0.0;
    bool b = false;

    assert_int_equal(json_req_get_str (req, "s",   &s),                         CUTILS_OK);
    assert_int_equal(json_req_get_u32 (req, "u32", &u32, 0, UINT32_MAX),        CUTILS_OK);
    assert_int_equal(json_req_get_u64 (req, "u64", &u64, 0, UINT64_MAX),        CUTILS_OK);
    assert_int_equal(json_req_get_i32 (req, "i32", &i32, INT32_MIN, INT32_MAX), CUTILS_OK);
    assert_int_equal(json_req_get_i64 (req, "i64", &i64, INT64_MIN, INT64_MAX), CUTILS_OK);
    assert_int_equal(json_req_get_f64 (req, "f64", &f,   -10.0, 10.0),          CUTILS_OK);
    assert_int_equal(json_req_get_bool(req, "b",   &b),                         CUTILS_OK);
    assert_true(json_req_is_null(req, "n"));

    assert_string_equal(s, "hello");
    assert_int_equal(u32, 100);
    assert_int_equal(u64, 200);
    assert_int_equal(i32, -300);
    assert_int_equal(i64, -400);
    assert_true(f > 2.49 && f < 2.51);
    assert_true(b);

    json_req_free(req);
}

static void test_resp_add_overwrites(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    assert_int_equal(json_resp_add_u32(resp, "port", 80U), CUTILS_OK);
    assert_int_equal(json_resp_add_u32(resp, "port", 443U), CUTILS_OK);

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);

    cutils_json_req_t *req = parse_or_fail(buf);
    uint32_t port = 0;
    assert_int_equal(json_req_get_u32(req, "port", &port, 0, UINT32_MAX), CUTILS_OK);
    assert_int_equal(port, 443);
    json_req_free(req);
}

static void test_resp_add_nested_creates_intermediates(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    assert_int_equal(json_resp_add_str(resp, "a.b.c.d", "deep"), CUTILS_OK);

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);

    cutils_json_req_t *req = parse_or_fail(buf);
    char *out __attribute__((cleanup(cutils_free_p))) = NULL;
    assert_int_equal(json_req_get_str(req, "a.b.c.d", &out), CUTILS_OK);
    assert_string_equal(out, "deep");
    json_req_free(req);
}

static void test_resp_add_through_non_object_errors(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    json_resp_add_u32(resp, "x", 1U);
    /* Now try to add "x.y" — x is a number, can't be traversed */
    int rv = json_resp_add_str(resp, "x.y", "oops");
    assert_int_equal(rv, CUTILS_ERR_JSON);
}

static void test_resp_add_u64_exceeds_2to53(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    int rv = json_resp_add_u64(resp, "n", 9007199254740993ULL);
    assert_int_equal(rv, CUTILS_ERR_JSON);
    assert_non_null(strstr(cutils_get_error(), "2^53"));
}

static void test_resp_add_i64_exceeds_2to53(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    int rv = json_resp_add_i64(resp, "n", -9007199254740993LL);
    assert_int_equal(rv, CUTILS_ERR_JSON);
}

static void test_resp_add_f64_rejects_nonfinite(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    assert_int_equal(json_resp_add_f64(resp, "x", NAN),      CUTILS_ERR_JSON);
    assert_int_equal(json_resp_add_f64(resp, "x", INFINITY), CUTILS_ERR_JSON);
    assert_int_equal(json_resp_add_f64(resp, "x", -INFINITY),CUTILS_ERR_JSON);
}

/* ============================================================ */
/* Response — array scalar append                                */
/* ============================================================ */

static void test_resp_array_append_scalars(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    assert_int_equal(json_resp_array_append_str (resp, "list", "a"),   CUTILS_OK);
    assert_int_equal(json_resp_array_append_u32 (resp, "list", 42U),   CUTILS_OK);
    assert_int_equal(json_resp_array_append_i32 (resp, "list", -7),    CUTILS_OK);
    assert_int_equal(json_resp_array_append_f64 (resp, "list", 1.5),   CUTILS_OK);
    assert_int_equal(json_resp_array_append_bool(resp, "list", true),  CUTILS_OK);
    assert_int_equal(json_resp_array_append_null(resp, "list"),        CUTILS_OK);
    assert_int_equal(json_resp_array_append_u64 (resp, "list", 9ULL),  CUTILS_OK);
    assert_int_equal(json_resp_array_append_i64 (resp, "list", -9LL),  CUTILS_OK);

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);

    cutils_json_req_t *req = parse_or_fail(buf);
    size_t n = 0;
    assert_int_equal(json_req_array_len(req, "list", &n), CUTILS_OK);
    assert_int_equal(n, 8);
    json_req_free(req);
}

static void test_resp_array_append_through_non_array_errors(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    json_resp_add_u32(resp, "x", 1U);
    int rv = json_resp_array_append_str(resp, "x", "oops");
    assert_int_equal(rv, CUTILS_ERR_JSON);
}

static void test_resp_array_append_u64_exceeds_rejected(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    int rv = json_resp_array_append_u64(resp, "xs", 9007199254740993ULL);
    assert_int_equal(rv, CUTILS_ERR_JSON);
}

static void test_resp_array_append_f64_nonfinite_rejected(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    assert_int_equal(json_resp_array_append_f64(resp, "xs", NAN), CUTILS_ERR_JSON);
}

/* ============================================================ */
/* Element builder                                               */
/* ============================================================ */

static void test_elem_build_and_commit(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    {
        cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
        assert_int_equal(json_resp_array_append_begin(resp, "items", &elem), CUTILS_OK);
        assert_int_equal(json_elem_add_str(&elem, "name", "widget"), CUTILS_OK);
        assert_int_equal(json_elem_add_u32(&elem, "count", 3U), CUTILS_OK);
        json_elem_commit(&elem);
    }

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);

    cutils_json_req_t *req = parse_or_fail(buf);
    size_t n = 0;
    json_req_array_len(req, "items", &n);
    assert_int_equal(n, 1);

    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    json_iter_begin(req, "items", &it);
    assert_true(json_iter_next(&it));

    char *name __attribute__((cleanup(cutils_free_p))) = NULL;
    uint32_t count = 0;
    json_iter_get_str(&it, "name", &name);
    json_iter_get_u32(&it, "count", &count, 0, 100);
    assert_string_equal(name, "widget");
    assert_int_equal(count, 3);
    json_req_free(req);
}

static void test_elem_discarded_when_not_committed(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    {
        cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
        assert_int_equal(json_resp_array_append_begin(resp, "items", &elem), CUTILS_OK);
        assert_int_equal(json_elem_add_str(&elem, "name", "lost"), CUTILS_OK);
        /* No commit call — cleanup discards */
    }

    /* Array still exists (begin created it) but is empty */
    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);

    cutils_json_req_t *req = parse_or_fail(buf);
    size_t n = 123;
    assert_int_equal(json_req_array_len(req, "items", &n), CUTILS_OK);
    assert_int_equal(n, 0);
    json_req_free(req);
}

static void test_elem_commit_then_cleanup_noop(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    {
        cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
        json_resp_array_append_begin(resp, "items", &elem);
        json_elem_add_u32(&elem, "x", 1U);
        json_elem_commit(&elem);
        /* cleanup runs next — must be idempotent */
    }

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);
    cutils_json_req_t *req = parse_or_fail(buf);
    size_t n = 0;
    json_req_array_len(req, "items", &n);
    assert_int_equal(n, 1);
    json_req_free(req);
}

static void test_elem_commit_multiple_times_idempotent(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    cutils_json_elem_t elem;
    json_resp_array_append_begin(resp, "items", &elem);
    json_elem_add_u32(&elem, "x", 1U);
    json_elem_commit(&elem);
    json_elem_commit(&elem);  /* no-op */
    json_elem_commit(&elem);  /* no-op */

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);
    cutils_json_req_t *req = parse_or_fail(buf);
    size_t n = 0;
    json_req_array_len(req, "items", &n);
    assert_int_equal(n, 1);
    json_req_free(req);
}

static void test_elem_cannot_modify_after_commit(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    cutils_json_elem_t elem;
    json_resp_array_append_begin(resp, "items", &elem);
    json_elem_commit(&elem);
    int rv = json_elem_add_str(&elem, "late", "nope");
    assert_int_equal(rv, CUTILS_ERR_INVALID);
}

static void test_elem_u64_exceeds_rejected(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
    json_resp_array_append_begin(resp, "items", &elem);
    int rv = json_elem_add_u64(&elem, "big", 9007199254740993ULL);
    assert_int_equal(rv, CUTILS_ERR_JSON);
}

static void test_elem_all_scalars(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    {
        cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
        json_resp_array_append_begin(resp, "items", &elem);
        assert_int_equal(json_elem_add_str  (&elem, "s",   "hi"),         CUTILS_OK);
        assert_int_equal(json_elem_add_u32  (&elem, "u32", 1U),           CUTILS_OK);
        assert_int_equal(json_elem_add_u64  (&elem, "u64", 2ULL),         CUTILS_OK);
        assert_int_equal(json_elem_add_i32  (&elem, "i32", -3),           CUTILS_OK);
        assert_int_equal(json_elem_add_i64  (&elem, "i64", -4LL),         CUTILS_OK);
        assert_int_equal(json_elem_add_f64  (&elem, "f64", 1.5),          CUTILS_OK);
        assert_int_equal(json_elem_add_bool (&elem, "b",   false),        CUTILS_OK);
        assert_int_equal(json_elem_add_null (&elem, "n"),                 CUTILS_OK);
        json_elem_commit(&elem);
    }

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    json_resp_finalize(resp, &buf, &len);
    cutils_json_req_t *req = parse_or_fail(buf);

    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    json_iter_begin(req, "items", &it);
    assert_true(json_iter_next(&it));

    char *s __attribute__((cleanup(cutils_free_p))) = NULL;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    double f = 0.0;
    bool b = true;
    json_iter_get_str (&it, "s",   &s);
    json_iter_get_u32 (&it, "u32", &u32, 0, UINT32_MAX);
    json_iter_get_u64 (&it, "u64", &u64, 0, UINT64_MAX);
    json_iter_get_i32 (&it, "i32", &i32, INT32_MIN, INT32_MAX);
    json_iter_get_i64 (&it, "i64", &i64, INT64_MIN, INT64_MAX);
    json_iter_get_f64 (&it, "f64", &f, -10.0, 10.0);
    json_iter_get_bool(&it, "b",   &b);
    assert_string_equal(s, "hi");
    assert_int_equal(u32, 1);
    assert_int_equal(u64, 2);
    assert_int_equal(i32, -3);
    assert_int_equal(i64, -4);
    assert_true(f > 1.49 && f < 1.51);
    assert_false(b);
    json_req_free(req);
}

static void test_elem_f64_nonfinite_rejected(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);
    cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
    json_resp_array_append_begin(resp, "items", &elem);
    assert_int_equal(json_elem_add_f64(&elem, "x", NAN), CUTILS_ERR_JSON);
}

/* ============================================================ */
/* End-to-end                                                    */
/* ============================================================ */

static void test_roundtrip_complex(void **state)
{
    (void)state;
    cutils_json_resp_t *resp __attribute__((cleanup(json_resp_free_p))) = NULL;
    json_resp_new(&resp);

    assert_int_equal(json_resp_add_str(resp, "meta.version", "1.0"), CUTILS_OK);
    assert_int_equal(json_resp_add_u32(resp, "meta.count",   2U),    CUTILS_OK);

    const char *names[]  = {"upspi", "upspi2"};
    const uint32_t runt[] = {1800U, 2400U};
    for (size_t i = 0; i < 2; i++) {
        cutils_json_elem_t elem __attribute__((cleanup(json_elem_commit_p)));
        assert_int_equal(json_resp_array_append_begin(resp, "ups_list", &elem), CUTILS_OK);
        assert_int_equal(json_elem_add_str(&elem, "name", names[i]), CUTILS_OK);
        assert_int_equal(json_elem_add_u32(&elem, "runtime_s", runt[i]), CUTILS_OK);
        json_elem_commit(&elem);
    }

    char *buf __attribute__((cleanup(cutils_free_p))) = NULL;
    size_t len = 0;
    assert_int_equal(json_resp_finalize(resp, &buf, &len), CUTILS_OK);

    /* Re-parse and verify every field */
    cutils_json_req_t *req = parse_or_fail(buf);

    char *ver __attribute__((cleanup(cutils_free_p))) = NULL;
    uint32_t count = 0;
    assert_int_equal(json_req_get_str(req, "meta.version", &ver), CUTILS_OK);
    assert_string_equal(ver, "1.0");
    assert_int_equal(json_req_get_u32(req, "meta.count", &count, 0, 10), CUTILS_OK);
    assert_int_equal(count, 2);

    cutils_json_iter_t it __attribute__((cleanup(json_iter_end_p)));
    json_iter_begin(req, "ups_list", &it);
    size_t idx = 0;
    while (json_iter_next(&it)) {
        char *nm __attribute__((cleanup(cutils_free_p))) = NULL;
        uint32_t rt = 0;
        json_iter_get_str(&it, "name", &nm);
        json_iter_get_u32(&it, "runtime_s", &rt, 0, UINT32_MAX);
        assert_string_equal(nm, names[idx]);
        assert_int_equal(rt, runt[idx]);
        idx++;
    }
    assert_int_equal(idx, 2);
    json_req_free(req);
}

/* ============================================================ */
/* NULL-arg robustness on public API                             */
/* ============================================================ */

static void test_null_arg_robustness(void **state)
{
    (void)state;
    char *out = NULL;
    uint32_t u32 = 0;
    bool b = false;
    size_t n = 0;

    /* Null req / null out on getters */
    assert_int_equal(json_req_get_str(NULL, "a", &out),                   CUTILS_ERR_INVALID);
    assert_int_equal(json_req_get_u32(NULL, "a", &u32, 0, 1),             CUTILS_ERR_INVALID);
    assert_int_equal(json_req_get_bool(NULL, "a", &b),                    CUTILS_ERR_INVALID);
    assert_int_equal(json_req_array_len(NULL, "a", &n),                   CUTILS_ERR_INVALID);

    /* Null resp on adders */
    assert_int_equal(json_resp_add_str(NULL, "a", "x"),                   CUTILS_ERR_INVALID);
    assert_int_equal(json_resp_add_u32(NULL, "a", 1U),                    CUTILS_ERR_INVALID);
    assert_int_equal(json_resp_array_append_str(NULL, "a", "x"),          CUTILS_ERR_INVALID);

    /* Null finalize args */
    assert_int_equal(json_resp_finalize(NULL, NULL, NULL),                CUTILS_ERR_INVALID);

    /* Null iter args */
    cutils_json_iter_t it;
    assert_int_equal(json_iter_begin(NULL, "a", &it),                     CUTILS_ERR_INVALID);

    /* json_iter_next on NULL must not crash */
    assert_false(json_iter_next(NULL));
}

/* ============================================================ */
/* Main                                                          */
/* ============================================================ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Parse / free */
        cmocka_unit_test_setup(test_parse_simple_object,         setup_clear_error),
        cmocka_unit_test_setup(test_parse_null_buf,              setup_clear_error),
        cmocka_unit_test_setup(test_parse_null_out,              setup_clear_error),
        cmocka_unit_test_setup(test_parse_malformed,             setup_clear_error),
        cmocka_unit_test_setup(test_parse_non_object,            setup_clear_error),
        cmocka_unit_test_setup(test_free_null_safe,              setup_clear_error),

        /* String getters */
        cmocka_unit_test_setup(test_get_str_happy,               setup_clear_error),
        cmocka_unit_test_setup(test_get_str_missing,             setup_clear_error),
        cmocka_unit_test_setup(test_get_str_wrong_type,          setup_clear_error),
        cmocka_unit_test_setup(test_get_str_opt_missing_returns_null,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_get_str_opt_present_returns_copy,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_get_str_opt_wrong_type_still_errors,
                                                                 setup_clear_error),

        /* Numeric getters */
        cmocka_unit_test_setup(test_get_u32_happy,               setup_clear_error),
        cmocka_unit_test_setup(test_get_u32_above_max,           setup_clear_error),
        cmocka_unit_test_setup(test_get_u32_below_min,           setup_clear_error),
        cmocka_unit_test_setup(test_get_u32_rejects_negative,    setup_clear_error),
        cmocka_unit_test_setup(test_get_u32_rejects_non_integer, setup_clear_error),
        cmocka_unit_test_setup(test_get_u32_wrong_type,          setup_clear_error),
        cmocka_unit_test_setup(test_get_u64_large_in_range,      setup_clear_error),
        cmocka_unit_test_setup(test_get_u64_exceeds_2to53,       setup_clear_error),
        cmocka_unit_test_setup(test_get_i32_happy_negative,      setup_clear_error),
        cmocka_unit_test_setup(test_get_i32_out_of_range,        setup_clear_error),
        cmocka_unit_test_setup(test_get_i64_exceeds_2to53,       setup_clear_error),
        cmocka_unit_test_setup(test_get_f64_happy,               setup_clear_error),
        cmocka_unit_test_setup(test_get_f64_out_of_range,        setup_clear_error),
        cmocka_unit_test_setup(test_get_bool_true,               setup_clear_error),
        cmocka_unit_test_setup(test_get_bool_false,              setup_clear_error),
        cmocka_unit_test_setup(test_get_bool_wrong_type,         setup_clear_error),

        /* Paths */
        cmocka_unit_test_setup(test_dotted_path_ok,              setup_clear_error),
        cmocka_unit_test_setup(test_path_missing_intermediate,   setup_clear_error),
        cmocka_unit_test_setup(test_path_non_object_intermediate,setup_clear_error),
        cmocka_unit_test_setup(test_path_empty_segment_rejected, setup_clear_error),

        /* Presence / structure */
        cmocka_unit_test_setup(test_has_present_and_missing,     setup_clear_error),
        cmocka_unit_test_setup(test_is_null,                     setup_clear_error),
        cmocka_unit_test_setup(test_array_len,                   setup_clear_error),
        cmocka_unit_test_setup(test_array_len_non_array,         setup_clear_error),

        /* Iterator */
        cmocka_unit_test_setup(test_iter_walks_array_of_objects, setup_clear_error),
        cmocka_unit_test_setup(test_iter_empty_array,            setup_clear_error),
        cmocka_unit_test_setup(test_iter_missing_path_errors,    setup_clear_error),
        cmocka_unit_test_setup(test_iter_non_array_errors,       setup_clear_error),
        cmocka_unit_test_setup(test_iter_get_before_next_errors, setup_clear_error),
        cmocka_unit_test_setup(test_iter_get_all_types,          setup_clear_error),

        /* Response lifecycle */
        cmocka_unit_test_setup(test_resp_new_empty_finalizes_to_empty_object,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_resp_free_null_safe,         setup_clear_error),

        /* Response adders */
        cmocka_unit_test_setup(test_resp_add_all_scalars,        setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_overwrites,         setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_nested_creates_intermediates,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_through_non_object_errors,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_u64_exceeds_2to53,  setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_i64_exceeds_2to53,  setup_clear_error),
        cmocka_unit_test_setup(test_resp_add_f64_rejects_nonfinite,
                                                                 setup_clear_error),

        /* Array scalar append */
        cmocka_unit_test_setup(test_resp_array_append_scalars,   setup_clear_error),
        cmocka_unit_test_setup(test_resp_array_append_through_non_array_errors,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_resp_array_append_u64_exceeds_rejected,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_resp_array_append_f64_nonfinite_rejected,
                                                                 setup_clear_error),

        /* Element builder */
        cmocka_unit_test_setup(test_elem_build_and_commit,       setup_clear_error),
        cmocka_unit_test_setup(test_elem_discarded_when_not_committed,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_elem_commit_then_cleanup_noop,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_elem_commit_multiple_times_idempotent,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_elem_cannot_modify_after_commit,
                                                                 setup_clear_error),
        cmocka_unit_test_setup(test_elem_u64_exceeds_rejected,   setup_clear_error),
        cmocka_unit_test_setup(test_elem_all_scalars,            setup_clear_error),
        cmocka_unit_test_setup(test_elem_f64_nonfinite_rejected, setup_clear_error),

        /* End-to-end + robustness */
        cmocka_unit_test_setup(test_roundtrip_complex,           setup_clear_error),
        cmocka_unit_test_setup(test_null_arg_robustness,         setup_clear_error),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
