#include "bench.h"
#include "cutils/json.h"
#include "cutils/error.h"
#include "cutils/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* End-to-end JSON request parse + field extraction, representative
 * of a daemon handling an inbound API call: parse the body once,
 * read a handful of fields by dot-path, free the request. Mirrors
 * the json_req_* usage shape sketched in the json.h header docstring. */

static const char *bench_json_input =
    "{"
    "  \"user\": {"
    "    \"name\": \"alice\","
    "    \"id\": 12345"
    "  },"
    "  \"action\": \"login\","
    "  \"timestamp\": 1700000000,"
    "  \"metadata\": {"
    "    \"ip\": \"192.168.1.100\","
    "    \"port\": 8443"
    "  }"
    "}";

void bench_json_parse_walk(bench_ctx_t *ctx)
{
    size_t json_len = strlen(bench_json_input);

    BENCH_LOOP(ctx) {
        cutils_json_req_t *req = NULL;
        if (json_req_parse(bench_json_input, json_len, &req) != CUTILS_OK) {
            fputs("bench_json: parse failed\n", stderr); exit(1);
        }

        char *name = NULL;
        char *action = NULL;
        char *ip = NULL;
        uint64_t id = 0, ts = 0;
        uint32_t port = 0;

        /* Read errors are not actionable inside a benchmark inner
         * loop — the inputs are known-valid. CUTILS_UNUSED actually
         * suppresses the warn_unused_result attribute (plain (void)
         * cast does not under gcc). */
        CUTILS_UNUSED(json_req_get_str (req, "user.name",    &name));
        CUTILS_UNUSED(json_req_get_u64 (req, "user.id",      &id, 0, UINT64_MAX));
        CUTILS_UNUSED(json_req_get_str (req, "action",       &action));
        CUTILS_UNUSED(json_req_get_u64 (req, "timestamp",    &ts, 0, UINT64_MAX));
        CUTILS_UNUSED(json_req_get_str (req, "metadata.ip",  &ip));
        CUTILS_UNUSED(json_req_get_u32 (req, "metadata.port",&port, 0, 65535));

        BENCH_USE(name); BENCH_USE(action); BENCH_USE(ip);
        BENCH_USE(id);   BENCH_USE(ts);     BENCH_USE(port);

        free(name); free(action); free(ip);
        json_req_free(req);
    }
}
