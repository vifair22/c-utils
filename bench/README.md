# c-utils benchmarks

Microbenchmark harness for the library's known hot paths. Drives the
optimization decisions for 1.1.0 and beyond — every `feature/perf-*` MR
in the 1.1.0 cycle carries before/after numbers from this harness in
its description.

## Running

```sh
make bench                  # full suite, release build
./build-bench/bin/bench     # re-run without rebuilding
./build-bench/bin/bench db  # filter by substring
```

Each run produces:

- A human-readable table on **stdout** (one row per benchmark).
- A CSV at **`build-bench/results.csv`** for `diff`-able before/after
  comparisons across optimization MRs.

## Methodology

- Built with **release flags** (`-O2`) in a dedicated `build-bench/`
  tree, separate from `build/` and `build-test/` — debug/coverage
  instrumentation never contaminates the numbers.
- Auto-calibration: each benchmark's iteration count is doubled until
  a single measurement pass takes at least **100 ms**, so timer
  overhead is dwarfed by the work being measured.
- **5 measurement samples** per benchmark; report **median**, min, max.
- Wall-clock from `CLOCK_MONOTONIC`. Run on a quiet machine for
  reproducible numbers (close browsers, kill background indexers, etc.).

## What each benchmark measures

| Benchmark | Hot path | Setup | What we're measuring |
|---|---|---|---|
| `config_get_str` | `config_get_str` worst-case | 300-key registry, look up the last key | Linear scan over the key array |
| `db_execute_1000_rows` | `db_execute` materialization | 1000 pre-loaded rows, 3 string columns each | sqlite step loop + 3000 per-cell `strdup`s + result-struct build |
| `db_iter_1000_rows` | `db_iter_*` streaming | Same fixture as `db_execute_1000_rows` | Iterator hands out sqlite's internal column pointers — no per-cell strdup, no row array; head-to-head against the materializing path |
| `log_write_db_off` | `log_write` filter short-circuit | `log_init(db=NULL, level=LOG_ERROR)`, then call `log_info` | The atomic-load + level-compare early return — the dominant cost of below-threshold log calls in a daemon |
| `log_write_db_on` | `log_write` full producer path | `log_init(db=open, level=LOG_INFO)`, then call `log_info` | `vsnprintf` + console format + async enqueue (DB writer drains concurrently, not measured here) |
| `json_parse_walk` | `json_req_parse` + getters | ~250-byte JSON, then read 6 fields by dot-path | cJSON parse + the request handle's dot-path traversal |
| `push_build_postfields` | URL-encoded POST body build | Representative title + 200-byte message, build once | `curl_easy_escape` of two strings + two-pass `snprintf` |

## Adding a new benchmark

1. Implement `bench_<name>(bench_ctx_t *ctx)` in `bench/bench_<subsystem>.c`.
   Setup outside the timed loop; use `BENCH_LOOP(ctx) { ... }` for the
   timed body; touch any computed values with `BENCH_USE(v)` so the
   optimizer doesn't dead-code-eliminate the work.
2. Declare the prototype in `bench/bench.h` so the strict-prototypes
   compiler regime sees a declaration before the definition.
3. Register the entry in `bench/bench_runner.c`'s `bench_table[]`.

## Baseline numbers (1.1.0)

Captured on commit `<bench-harness HEAD>` against the pre-optimization
codebase. Hardware: Gentoo Linux on x86_64, gcc 13.x, gcovr-irrelevant
(release build, no instrumentation). Re-running these is the way to
spot a regression — any optimization MR must reproduce numbers in the
same ballpark or better.

### 1.1.0 baseline (final)

Captured at the close of the 1.1.0 cycle. Hardware: Gentoo Linux on
x86_64, gcc 13.x, release build (`-O2`).

| Benchmark | Median (ns) | Min (ns) | Max (ns) |
|---|---:|---:|---:|
| `config_get_str` | 67 | 65 | 69 |
| `db_execute_1000_rows` | 199761 | 197485 | 201856 |
| `db_iter_1000_rows` | 138493 | 138245 | 140576 |
| `log_write_db_off` | 2.3 | 2.3 | 2.3 |
| `log_write_db_on` | 9594 | 9273 | 9689 |
| `json_parse_walk` | 752 | 749 | 761 |
| `push_build_postfields` | 1125 | 1123 | 1135 |

### Changes vs the cycle's pre-optimization baseline

| Benchmark | 1.1.0 start | 1.1.0 final | Delta |
|---|---:|---:|---:|
| `config_get_str` | 1889 | 67 | **−96.5%** (28× faster) |
| `db_iter_1000_rows` | — (new in 1.1.0) | 138493 | new API, **28% faster than `db_execute`** on the same fixture |
| Others | within ±5% bench-to-bench noise | | no targeted change |

The `db_iter_*` row is the most informative head-to-head: same fixture
and column shape as `db_execute_1000_rows`, but consumed via the
streaming iterator. The iterator runs **~31% faster** for this size
AND uses O(1) heap regardless of row count, where `db_execute` is
O(n) — every cell is `strdup`'d into an owned result struct. The new
API is the right choice for SELECTs over unknown or large row counts;
`db_execute` remains the right call for known-small result sets you
want to pass around as an indexable array.

### Hot-path optimization status

| Bench | Status | Notes |
|---|---|---|
| `config_get_str` | **Optimized 28×** | Two stacked bsearch optimizations — `cfg->keys` (MR !39) and `doc->entries` (MR !40) |
| `db_execute_1000_rows` | Already-optimal-per-bench for typical cell sizes | A strpool-based optimization was tried and rejected: 7% regression on small (~10 byte) cells, because glibc's small-bin allocator fast-paths individual strdups faster than maintaining a growing arena + offset table + fixup pass. Would likely win for larger cells, but would regress the common case. The new `db_iter_*` API is the right answer for large-row queries. |
| `log_write_db_off` | Already-optimal-per-bench | 2.3 ns is the `_Atomic` level-compare short-circuit. Filtered-out log calls cost almost nothing; this is the expected behavior. |
| `log_write_db_on` | Already-optimal-per-bench | 9.6 µs is dominated by synchronous `fprintf` to stdout, the mutex lock for queue insert, and the cond_signal for the writer thread — all kernel-side costs we can't optimize at this level. A flexible-array-member micro-optimization (one alloc instead of two) was tried and produced only 0.7% improvement, below the merge bar. |
| `json_parse_walk` | Already-optimal-per-bench | 752 ns is the cJSON parse + 6 dot-path lookups; cJSON's internal lookup is itself a linear scan over the parsed children, but the object is small (6 fields). Replacing cJSON with a faster parser is a different conversation about the JSON dependency, not an optimization within the wrapper. |
| `push_build_postfields` | Already-optimal-per-bench | 1.1 µs is dominated by two `curl_easy_escape` calls (libcurl URL-encoding) + a two-pass snprintf. URL-encoding is libcurl's job; the build itself is already minimal. |

The "already-optimal-per-bench" entries are not declarations of theoretical optimality — they're statements that the dominant cost lives outside c-utils' code (libc, libcurl, sqlite, cJSON, kernel) and any user-space attempt to optimize the wrapper has been either tried-and-rejected or shown to have insignificant leverage by inspection.

Quick reading: `log_write_db_off` at 2.3 ns is the atomic-load
short-circuit doing its job — that's effectively a no-op call and
hard to improve. `db_execute_1000_rows` at 192 μs is the most
expensive single operation by far and the prime optimization target;
of that, ~3000 `strdup` calls per query are the obvious focus.
`config_get_str` at 1.8 μs over a 300-key registry is the most
visible per-call cost in a daemon that reads config inside a hot loop.

## Caveats

- **Microbenchmarks lie.** A 5% improvement on a synthetic loop may
  not translate to a measurable end-to-end win. The optimization
  merge bar requires the bench number to move and no other bench to
  regress, but the final decision is always "is this complexity
  worth the win" — judgement, not just arithmetic.
- The harness pins nothing (CPU frequency, scheduler affinity).
  Cross-machine numbers are not comparable; only same-machine
  before/after numbers are.
- `log_write_db_on` measures producer cost only; the async DB writer
  drains in parallel on its own thread and is not factored in.
