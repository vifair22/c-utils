#include "bench.h"
#include "cutils/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Worst-case config_get_str: 300-key registry, look up the very last
 * key so the linear scan in config_get_key_def walks every entry. The
 * hit path is the file-backed branch (no env override, no DB), which
 * is the dominant call shape for daemon-resident config reads. */

#define BENCH_CFG_NKEYS    300
#define BENCH_CFG_PATH     "/tmp/cutils_bench_config.yaml"

static char target_key[64];

static void write_yaml_with_n_keys(int n)
{
    FILE *f = fopen(BENCH_CFG_PATH, "w");
    if (!f) { perror("bench_config: fopen"); exit(1); }
    fputs("db:\n  path: bench.db\n", f);
    for (int i = 0; i < n; i++)
        fprintf(f, "s%d:\n  k%d: v%d\n", i, i, i);
    fclose(f);
}

static config_key_t *build_keys_array(int n)
{
    config_key_t *keys = calloc((size_t)n + 1, sizeof(*keys));
    if (!keys) { fputs("bench_config: alloc\n", stderr); exit(1); }
    for (int i = 0; i < n; i++) {
        char *k = malloc(32);
        if (!k) { fputs("bench_config: alloc\n", stderr); exit(1); }
        snprintf(k, 32, "s%d.k%d", i, i);
        keys[i].key           = k;
        keys[i].type          = CFG_STRING;
        keys[i].default_value = "default";
        keys[i].description   = "";
        keys[i].store         = CFG_STORE_FILE;
        keys[i].required      = 0;
    }
    return keys;
}

static void free_keys_array(config_key_t *keys, int n)
{
    for (int i = 0; i < n; i++)
        free((void *)keys[i].key);
    free(keys);
}

void bench_config_get_str(bench_ctx_t *ctx)
{
    write_yaml_with_n_keys(BENCH_CFG_NKEYS);
    config_key_t *keys = build_keys_array(BENCH_CFG_NKEYS);

    cutils_config_t *cfg = NULL;
    int rc = config_init(&cfg, "benchapp", BENCH_CFG_PATH,
                         CFG_FIRST_RUN_CONTINUE, keys, NULL);
    if (rc != 0) { fputs("bench_config: config_init failed\n", stderr); exit(1); }

    /* Last key in the array — worst case for the linear scan. */
    snprintf(target_key, sizeof(target_key),
             "s%d.k%d", BENCH_CFG_NKEYS - 1, BENCH_CFG_NKEYS - 1);

    BENCH_LOOP(ctx) {
        const char *v = config_get_str(cfg, target_key);
        BENCH_USE(v);
    }

    config_free(cfg);
    free_keys_array(keys, BENCH_CFG_NKEYS);
    unlink(BENCH_CFG_PATH);
}
