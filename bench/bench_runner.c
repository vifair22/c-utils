#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Auto-calibration target: each measurement run must take at least
 * this many seconds so timing overhead (clock_gettime + the harness
 * loop itself) is dwarfed by the work being measured. */
#define BENCH_MIN_RUNTIME_SECONDS 0.1

/* How many measurement samples we take after calibration. Median is
 * the headline number; min/max are reported for variance context. */
#define BENCH_SAMPLES 5

/* Safety cap on iteration count — protects against benchmarks whose
 * body is so cheap that auto-calibration would otherwise loop into
 * the billions and consume gigabytes of memory if the body allocates. */
#define BENCH_MAX_ITERS (1u << 28)

struct bench_ctx {
    size_t iters;
};

size_t bench_iters(bench_ctx_t *ctx) { return ctx->iters; }

struct bench_entry {
    const char *name;
    void     (*fn)(bench_ctx_t *);
};

/* Add new benchmarks here. The order is the order they run in. */
static const struct bench_entry bench_table[] = {
    { "config_get_str",          bench_config_get_str        },
    { "db_execute_1000_rows",    bench_db_execute_1000_rows  },
    { "log_write_db_off",        bench_log_write_db_off      },
    { "log_write_db_on",         bench_log_write_db_on       },
    { "json_parse_walk",         bench_json_parse_walk       },
    { "push_build_postfields",   bench_push_build_postfields },
};

static const size_t bench_table_len =
    sizeof(bench_table) / sizeof(bench_table[0]);

static double time_one(void (*fn)(bench_ctx_t *), size_t iters)
{
    struct timespec t0, t1;
    bench_ctx_t ctx = { .iters = iters };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fn(&ctx);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    return secs;
}

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static void run_one(const struct bench_entry *e, FILE *csv)
{
    /* Calibration: start at 1 iter and double until single-run
     * duration crosses BENCH_MIN_RUNTIME_SECONDS or we hit the safety
     * cap. */
    size_t iters = 1;
    double elapsed = time_one(e->fn, iters);
    while (elapsed < BENCH_MIN_RUNTIME_SECONDS && iters < BENCH_MAX_ITERS) {
        iters *= 2;
        elapsed = time_one(e->fn, iters);
    }

    /* Measurement: BENCH_SAMPLES timed runs at the calibrated count. */
    double samples[BENCH_SAMPLES];
    for (int i = 0; i < BENCH_SAMPLES; i++)
        samples[i] = time_one(e->fn, iters);

    qsort(samples, BENCH_SAMPLES, sizeof(samples[0]), compare_double);

    double per_iter_min = samples[0]                  * 1e9 / (double)iters;
    double per_iter_med = samples[BENCH_SAMPLES / 2]  * 1e9 / (double)iters;
    double per_iter_max = samples[BENCH_SAMPLES - 1]  * 1e9 / (double)iters;

    printf("%-30s iters=%-10zu median=%10.1f ns  min=%10.1f ns  max=%10.1f ns\n",
           e->name, iters, per_iter_med, per_iter_min, per_iter_max);

    if (csv)
        fprintf(csv, "%s,%zu,%.3f,%.3f,%.3f\n",
                e->name, iters, per_iter_med, per_iter_min, per_iter_max);
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [filter-substring]\n"
        "\n"
        "Runs every registered benchmark whose name contains the optional\n"
        "filter substring (or all of them when no filter is given).\n"
        "\n"
        "Output: human-readable table on stdout, CSV at build-bench/results.csv.\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        filter = argv[1];
    }

    /* CSV output for before/after diffing. Ignore creation errors —
     * stdout still gets the report. */
    (void)mkdir("build-bench", 0755);
    FILE *csv = fopen("build-bench/results.csv", "w");
    if (csv)
        fprintf(csv, "name,iters,median_ns,min_ns,max_ns\n");

    printf("=== c-utils benchmarks ===\n");
    for (size_t i = 0; i < bench_table_len; i++) {
        if (filter && !strstr(bench_table[i].name, filter))
            continue;
        run_one(&bench_table[i], csv);
    }

    if (csv) fclose(csv);
    return 0;
}
