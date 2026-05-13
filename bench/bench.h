#ifndef CUTILS_BENCH_H
#define CUTILS_BENCH_H

#include <stddef.h>

/* --- c-utils benchmark harness ---
 *
 * Each benchmark is a function:
 *
 *   void bench_my_thing(bench_ctx_t *ctx) {
 *       // setup (untimed)
 *       ...
 *       BENCH_LOOP(ctx) {
 *           // the body the harness times
 *           hot_call();
 *       }
 *       // teardown (untimed)
 *       ...
 *   }
 *
 * The runner auto-calibrates iteration count per benchmark so each
 * measurement run takes at least 100 ms, then takes 5 measurement
 * samples and reports median / min / max nanoseconds per iteration.
 *
 * Output is one row per benchmark to stdout (human-readable) and a
 * CSV at build-bench/results.csv (machine-readable, for diffing
 * before/after across optimization MRs).
 *
 * Add a new benchmark by:
 *   1. Implement bench_<name> in bench/bench_<subsystem>.c
 *   2. Declare bench_<name> in this header (so the strict-prototypes
 *      regime sees a declaration before the definition)
 *   3. Register the entry in bench_runner.c's bench_table */

typedef struct bench_ctx bench_ctx_t;

/* The harness-provided iteration count for the current measurement
 * pass. Use BENCH_LOOP rather than calling this directly. */
size_t bench_iters(bench_ctx_t *ctx);

/* Benchmark prototypes. */
void bench_config_get_str       (bench_ctx_t *ctx);
void bench_db_execute_1000_rows (bench_ctx_t *ctx);
void bench_log_write_db_off     (bench_ctx_t *ctx);
void bench_log_write_db_on      (bench_ctx_t *ctx);
void bench_json_parse_walk      (bench_ctx_t *ctx);
void bench_push_build_postfields(bench_ctx_t *ctx);

/* Run the body once per harness-provided iteration. The body must
 * have at least one side effect or use BENCH_USE on a computed value,
 * otherwise the optimizer is free to delete it entirely. */
#define BENCH_LOOP(ctx) \
    for (size_t _bench_i = 0, _bench_n = bench_iters(ctx); \
         _bench_i < _bench_n; \
         _bench_i++)

/* Touch a value through an inline-asm barrier so the optimizer can't
 * dead-code-eliminate the computation that produced it. */
#define BENCH_USE(v) __asm__ __volatile__("" : : "r"(v) : "memory")

#endif
