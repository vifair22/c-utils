#include "bench.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

/* Push's CPU-bound hot path is the URL-encoded body builder. The
 * worker-side send_one calls cutils_push_build_postfields once per
 * message; everything else in send_one is libcurl (network-bound).
 * Bench at the helper level so we measure pure CPU and don't get
 * confused by network variability or DB insert cost.
 *
 * The benchmark uses representative inputs: a short title, a
 * mid-sized message (~200 bytes), realistic-shaped token/user
 * strings. URL-encoding work scales with the message length, so a
 * 200-byte sample is the median call shape. */

extern char *cutils_push_build_postfields(CURL *curl,
                                          const char *token, const char *user,
                                          const char *title, const char *message,
                                          const char *timestamp, const char *ttl,
                                          int html, int priority);

void bench_push_build_postfields(bench_ctx_t *ctx)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) { fputs("bench_push: curl_easy_init\n", stderr); exit(1); }

    const char *token = "abcdefghijklmnopqrstuvwxyz012345";
    const char *user  = "zyxwvutsrqponmlkjihgfedcba543210";
    const char *title = "UPS event: battery on";
    const char *msg   = "Battery on at 14:23:01. Load: 38%. Estimated runtime: "
                        "18 minutes. Input voltage: 119.4V. Output: 120.0V. "
                        "Last self-test: 2026-04-12.";
    const char *ts    = "1700000000";
    const char *ttl   = "86400";

    BENCH_LOOP(ctx) {
        char *body = cutils_push_build_postfields(
            curl, token, user, title, msg, ts, ttl, 0, 0);
        if (!body) { fputs("bench_push: build failed\n", stderr); exit(1); }
        BENCH_USE(body);
        free(body);
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
}
