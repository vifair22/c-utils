# Changelog

All notable changes to c-utils are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) at
the public-header level (see README "API stability").

## [1.8.2] - 2026-11-19

Migration error context for c-backup (B288), and a Makefile fix.

### Fixed

- **Batch-level migration failures had no context.** A COMMIT failing
  with `SQLITE_FULL` reported only `exec: database or disk is full`.
  The tracking table setup, BEGIN, checksum lookup, recording and
  COMMIT now report `<step> (sqlite error <code>): <message>` and
  return `CUTILS_ERR_MIGRATE` (previously the bare `CUTILS_ERR_DB`).
  - A COMMIT failure names the last migration applied in the batch.
  - Checksum lookup and recording failures name their migration.
- **`make` failed with "can't create build/vendor/cJSON.o"** when
  `build/` existed without its subdirectories; the same applied to
  `build-test/` and `build-bench/`. Each rule now creates the
  directory it writes into.

## [1.8.1] - 2026-11-18

One fix to migration error reporting, for c-backup (B244).

### Fixed

- **A migration that failed after SQLite rolled the transaction back
  reported the wrong error.** Temp-store failures, `SQLITE_FULL`,
  `SQLITE_IOERR` and `RAISE(ROLLBACK)` make SQLite end the transaction
  itself. The runner's `ROLLBACK TO` and the scoped tx cleanup's
  `ROLLBACK` then failed, and "cannot rollback - no transaction is
  active" replaced the statement's own error.
  - Both rollbacks now run only while a transaction is still open.
  - A failed migration reports
    `migration <name> failed (sqlite error <code>): <message>`, with
    SQLite's extended result code.
  - `CUTILS_AUTO_DB_TX` cleanup keeps the caller's error message for
    every caller, not only migrations.

## [1.8.0] - 2026-11-17

An application write lock on the db handle, for c-backup's write audit
(CUTILS-2). Additive; without a lock installed nothing changes.

### Added

- **`db_set_write_lock(db, lock, unlock, ctx)`** hands the application's
  own transaction lock to the handle. SQLite keeps one transaction per
  connection, so an app running raw `BEGIN ... COMMIT` under its own
  lock left a window in which c-utils' writers ran inside its
  transaction.
  - Every c-utils write scope now takes the app lock, then the handle
    mutex: the log writer (per batch, and retention cleanup),
    `push_send`, the push worker's reads and updates, and the scoped
    `cutils_db_tx_begin*`.
  - `lock` returns `DB_WRITE_LOCK_ACQUIRED`, `DB_WRITE_LOCK_HELD` (this
    thread already holds it; the write joins its transaction and unlock
    is not called), or a negative value to refuse (`CUTILS_ERR_DB`).
- **`db_write_begin` / `db_write_end`** (`CUTILS_AUTO_DB_WRITE`) and
  **`db_has_write_lock`**, for application code.

### Changed

- `cutils_db_tx_t` gains a `wg` field. Callers that zero-initialise it
  (`= { 0 }`) need only a rebuild.
- With a write lock installed, AppGuard's signal-driven exit no longer
  drains the log and push workers, so an app thread wedged while
  holding the lock cannot hang the exit. Queued push rows are durable
  and go out on the next start.

### Fixed

- **A multi-part `push_send` inside an open transaction failed** with
  "cannot start a transaction within a transaction". The parts now go
  in under a savepoint.

## [1.7.2] - 2026-11-17

One AppGuard fix, found by c-backup's TSan sweep.

### Fixed

- **AppGuard's signal-driven exit closed the DB under live threads.**
  On SIGINT/SIGTERM (the default path, or the second signal when
  `on_signal` is set) the watcher called `db_close` while application
  threads could still be inside a `db_*` call, destroying the handle
  mutex and freeing the handle mid-query.
  - The watcher now stops only the log and push workers and exits
    with the DB open. Committed transactions are durable and SQLite
    recovers the WAL on next open, as after a crash.
  - `appguard_shutdown` still closes the DB.
  - The exit is still `exit(0)`, so `atexit` handlers run; other
    threads are alive while they do, now noted in the Manual.

## [1.7.1] - 2026-11-16

One fix to the 1.7.0 typed binds, found by c-backup while adopting them.

### Fixed

- **`DB_BLOB` with a NULL pointer read out of bounds.** 1.7.0 replaced
  a NULL pointer with a 1-byte `""` but kept the given length, so
  `DB_BLOB(NULL, 32)` made sqlite copy 32 bytes from it, and the bound
  value was garbage rather than NULL.
  - A NULL pointer now binds SQL NULL whatever the length, the rule
    `DB_TEXT(NULL)` already followed. `DB_BLOB(have ? hash : NULL, 32)`
    binds the hash or NULL.
  - **Behaviour change:** `DB_BLOB(NULL, 0)` now binds NULL too; in
    1.7.0 it bound an empty blob. For an empty blob, pass any non-NULL
    pointer with length 0.

## [1.7.0] - 2026-11-16

A prepared-statement cache in the db layer, typed parameter binding and
NULL detection in results, all for c-backup. Purely additive at the API
level; the cache is on by default.

### Added

- **Prepared-statement cache.** `db_execute`, `db_execute_non_query`
  and the new `_typed` calls keep prepared statements per handle in a
  bounded LRU keyed by the exact SQL text, under the existing handle
  mutex. A repeat call skips the parse and plan.
  - Statements are reused through `sqlite3_reset`, so no read
    transaction is held between calls.
  - `sqlite3_clear_bindings` is never called. An omitted trailing
    parameter still reads as NULL.
  - A schema change is handled: sqlite re-prepares, and a residual
    `SQLITE_SCHEMA` re-prepares once.
  - Any error drops the entry, and `db_close` finalizes everything.
  - `db_stmt_cache_set_size` sets the size (default 32, max 4096,
    0 turns it off). `db_stmt_cache_stats` reports the counters.
- **`db_execute_once`, `db_execute_non_query_once`**: the same calls
  without the cache, for one-off or variable-text SQL.
- **Typed parameters**: `db_param_t` with `DB_TEXT`, `DB_BLOB`,
  `DB_INT64`, `DB_DOUBLE` and `DB_NULLP`, passed as a counted array to
  `db_execute_typed` and `db_execute_non_query_typed`.
  - BLOBs keep embedded zero bytes.
  - SQL NULL can be bound.
  - The count replaces the "NULL ends the array" terminator.
- **`db_result_is_null(result, row, col)`** tells an SQL NULL cell from
  an empty string. Both read as `""` in `rows`.

### Changed

- `db_execute` reads the column count after the first step instead of
  after prepare, so a cached `SELECT *` follows an `ALTER TABLE`.
- Results are allocated inside a private wrapper that carries the NULL
  flags. The public `db_result_t` layout is unchanged.
  `db_result_free` and `db_result_is_null` accept only results
  returned by the library, which was already the only supported use.

### Performance

Release build, from `make bench`:

| Benchmark | 1.6.0 | 1.7.0 |
|---|---|---|
| `db_per_file_8_once` (8 point statements, no cache) | n/a | 15.5 us |
| `db_per_file_8_cached` (same SQL, cached) | n/a | 5.1 us |
| `db_typed_blob_lookup` (32-byte BLOB key) | n/a | 0.53 us |
| `db_execute_1000_rows` | 158 us | 157 us |
| `db_iter_1000_rows` | 95 us | 92 us |

The cache saves about 10 us per 8-statement unit of work here, roughly
1 s per 100k files on a simple schema. Statements that cost more to
plan save more. One exception: with STAT4 and `ANALYZE`, sqlite itself
re-prepares a statement whose plan depends on a bound value whenever
that value changes. The cache cannot avoid that, and never adds a
re-prepare of its own.

## [1.6.0] - 2026-11-15

`json_req_parse` is strict about what it accepts. No signature changes,
but bodies that parsed under 1.5.x can now fail, hence the minor bump.
Found in c-backup's UAT.

### Changed

- **Behaviour change: `json_req_parse` refuses data after the JSON
  value.** It used to succeed once cJSON had read one value from the
  front of the buffer, so `{"a":1} junk` and `{"a":1}{"b":2}` were
  accepted with part of the body never read. Now all `len` bytes must be
  the one object; only JSON whitespace (space, tab, CR, LF) may follow
  it. A NUL counted in `len` is data and is refused, so a caller passing
  `sizeof("...")` rather than the string length now fails. The result is
  `CUTILS_ERR_JSON`, the code a malformed body already returned (a 400
  for an HTTP service), with `unexpected data after the JSON value, at
  byte N: ...`.
- **Behaviour change: `json_req_parse` refuses duplicate member names**
  in any object at any depth. `{"role":"user","role":"admin"}` used to
  parse, the getters returning the first while a proxy, validator or
  logger in front of the service may honour the last. It is now
  `CUTILS_ERR_JSON` with the key and its location, as in `duplicate key
  'x' in object 'items[1]'`. Names compare byte-for-byte, as the getters
  look them up, so `"a"` and `"A"` are different names. The check is
  O(n log n) per object, and costs about 60 ns on a small request.

### Fixed

- **A parse error could read past the end of the buffer.** The `near:`
  excerpt was printed with `%.32s`, which reads until a NUL, and an HTTP
  body has none. It is now bounded by `len`.

## [1.5.1] - 2026-11-15

Three fixes found in c-backup's UAT against 1.5.0: a data race in JSON
parsing, and two to what is reported (NIT-4, NIT-5). No API or ABI
changes.

### Fixed

- **Concurrent JSON parses raced, and could crash.** The vendored cJSON
  kept its parse error position in a plain global written by every parse,
  so `json_req_parse` on two threads at once raced on it (ThreadSanitizer
  reports it). A failed parse could then report a position inside
  another thread's buffer, or dereference a torn pointer and crash. The
  global is now `_Thread_local`, a marked local change to
  `lib/cJSON/cJSON.c`.
- **Config errors printed twice under AppGuard.** Validation prints each
  problem, then keeps the first in the error buffer for callers that print
  nothing themselves; `appguard_init` printed that buffer as well, so an
  invalid `log.level` appeared once from validation and again as
  `appguard: config init failed: ...`. With validation on, AppGuard now
  says init failed and points at the list above it. With validation off,
  nothing else reports the error, so the buffer is still printed. The
  buffer's text is unchanged.
- **The push retry line gave no cause.** A transient failure logged the
  attempt and the delay only; the HTTP status and body reached the
  permanent-failure line alone, and a curl error was dropped on every
  path. Both lines now carry the cause: curl's error message when there
  was no response, otherwise `HTTP <status>, <start of body>`, as in
  `push transient failure (HTTP 503, {...}; attempt 1, retry in 30s):
  <title>`. The permanent line reads as before.

## [1.5.0] - 2026-11-15

Closes the gaps c-backup's UAT found in 1.4.0's config validation (B081
reopened). One addition, opt-in: an application check hook. No signature
changes; an application that sets nothing new sees only the fixes below.

### Added

- **Application checks: `config_check_fn`, `config_init_opts_t.check`,
  `appguard_config_t.config_check`**, with `config_check_fail()`,
  `config_check_int_range()` and `config_check_one_of()`. Types say
  nothing about meaning: `port: 99999` is a valid integer. An application
  could only check after init returned, and only if it succeeded, so one
  run reported the file's problems or the application's, never both. The
  hook runs inside validation and its problems join the same list, sorted
  by line and located like built-in ones (file line, environment
  variable, or default).
- **`log_level_parse()`** - the level names AppGuard and validation both
  use, so they cannot disagree.

### Fixed

- **`log.level` dropped from the problem list.** AppGuard checked it after
  config init, so with any other problem in the file init failed first and
  the bad level was never mentioned. It is now checked in the validation
  pass.
- **Required keys reported alone, without the file.** A missing
  required key is now one more problem in the list, as
  `<path>: required config key 'db.path' is missing or empty; it has no
  default`, and an environment override satisfies it as it does every
  read.
- **CRLF line endings and a UTF-8 byte order mark** are accepted. A blank
  CRLF line was `"\r"`, not blank, and was reported as `'' is not a section
  header` with the CR invisible; a BOM glued itself to the first section
  name. A rewrite by `config_set` writes LF without the mark.

### Changed

- **Tabs in indentation are a validation problem**, as YAML says, reported
  once per line. 1.4.0 counted a tab as one column. Under
  `CFG_VALIDATE_OFF` the line still loads as before.

## [1.4.0] - 2026-11-15

Lets an application refuse a config file it cannot honour, and count
Pushover deliveries. Both are opt-in: no signature changes, and an
application that sets nothing new behaves as under 1.3.1 apart from the
fixes below. Found in c-backup's UAT (B081, B083).

### Added

- **Config validation: `config_validate_t`, `config_init_opts()`,
  `appguard_config_t.config_validate`.** Until now a config file was
  accepted whatever it said: `log: [unclosed` loaded as if the line were
  not there, `security.key_dri` was just an unread entry, and
  `port: eighty` read as the default. With `CFG_VALIDATE_WARN` each
  problem is printed as `<path>:<line>: warning: ...`; with
  `CFG_VALIDATE_STRICT` they are printed as errors and init fails with
  `CUTILS_ERR_CONFIG` (AppGuard: before the database is opened). Checked:
  - lines outside the supported YAML subset, now documented in Manual
    section 5: flow collections, nested mappings, top-level scalars,
    inconsistent indentation, single or unbalanced quotes, anchors and
    block scalars, `: ` in an unquoted value, keys outside a section,
    duplicate keys, lines over 1023 bytes;
  - unknown keys, with a suggestion when a registered key is within two
    edits (`did you mean 'security.key_dir'?`);
  - DB-backed keys written in the file, where they have no effect;
  - `CFG_INT` / `CFG_BOOL` values, in the file and in environment
    overrides;
  - under AppGuard, `log.level`.
  The default, `CFG_VALIDATE_OFF`, keeps 1.3 behaviour; the parser produces
  exactly the entries it did before and only records the problems.
- **`config_key_line()`** - the line a key was set on, for application
  checks that want to point at it.
- **`push_get_stats()` / `push_stats_t`** - lock-free sent, failed,
  expired and retried counters plus the pending queue depth. A message
  Pushover rejected, or one that expired, used to leave only a log line;
  an application can now export it as a metric and alert on it.

### Fixed

- **A version bump now rebuilds the object that reports the version.**
  `version.c` takes the version from `release_version` through a compiler
  define, so make never saw the dependency: an incremental build after a
  bump kept the old `version.o`, and `cutils_version()` reported the
  previous release. Found when c-backup's daemon logged c-utils 1.2.0
  while linked against 1.3.1.
- **`log.level: warn` is honoured.** AppGuard knew `warning` but not
  `warn`, the spelling most applications document, and fell back to the
  compiled-in level without a word.
- **A rejected push says why.** The log line now carries the HTTP status
  and the start of Pushover's response body:
  `push permanently failed (HTTP 400, {"user":"invalid",...}): <title>`.

## [1.3.1] - 2026-11-15

### Fixed

- **A read-only config file no longer stops the application.** With
  `config_mode` set, AppGuard chmod'ed the config on every start. A config
  bind-mounted read-only into a container, the usual way to supply one,
  failed that chmod with EROFS, and `appguard_init` returned NULL: the
  daemon exited and its supervisor restarted it forever, though the file
  was already at the required mode. Found in c-backup's UAT on Unraid.
  - A file already at `config_mode` is left alone.
  - A file at the wrong mode whose filesystem refuses the change (EROFS,
    EPERM) gets a warning on stderr naming both modes, and init continues:
    refusing to start cannot fix it.
  - Any other chmod failure is still fatal.

## [1.3.0] - 2026-11-15

Lets a daemon own its shutdown. One new optional `appguard_config_t`
pair; no signatures change and an application that does not set it
behaves exactly as under 1.2.0.

### Added

- **`appguard_config_t.on_signal` / `on_signal_ctx`** - when set, the
  first SIGINT or SIGTERM calls `on_signal(sig, ctx)` on the signal
  watcher thread instead of draining the subsystems and calling
  `exit(0)`. The application stops its own work in its own order and
  then calls `appguard_shutdown()`. A second SIGINT or SIGTERM takes
  the default forced exit, so a wedged shutdown can still be stopped.

  Without it there was no way for an application to run its own
  teardown on a signal: the signals are blocked in every thread and
  the watcher receives them, so a handler installed with `signal()` or
  `sigaction()` never ran. c-backupd had one, and `systemctl stop`
  closed the database underneath running backup jobs without the
  daemon's shutdown path ever executing.

### Fixed

- **Manual: signal handling.** The documented way to run custom
  cleanup, installing a `signal()` handler after `appguard_init()`,
  could not work for the reason above. Rewritten around `on_signal`.

## [1.2.0] - 2026-05-13

Security-focused minor release. Wires the v1.1.0 `db_open_with_mode`
primitive through the AppGuard lifecycle manager so daemons get
end-to-end perm enforcement on the DB, its sqlite sidecars, and the
config file with three new `appguard_config_t` fields and zero
application-side umask gymnastics. No public-header function
signatures change; the additions are two new fields and one new
accessor.

### Added

- **`appguard_config_t.db_mode`** — when nonzero, the database file
  and its `.db-wal` / `.db-shm` sidecars all end up at this mode by
  the time `appguard_init` returns. Internally, AppGuard narrows the
  process umask to `~db_mode & 0777` for the duration of the DB-open
  and migration phase (so sqlite materializes the sidecars with the
  right perms in the first place), then chmod's all three artifacts
  explicitly (idempotent — handles pre-existing files from a prior
  run under a more permissive umask), and finally restores the
  caller's original umask before returning. The umask change is
  scoped to internal init steps only and does **not** bleed into the
  running application — files the daemon creates after init still
  honor whatever umask the app sets. Documented mid-session edge
  case: if an external process deletes a sidecar and sqlite recreates
  it, the new file inherits the app's then-current umask; daemons
  needing protection against this rare case should set `umask(0077)`
  themselves at startup.
- **`appguard_config_t.config_mode`** — when nonzero, the YAML config
  file is chmod'd to this mode after `config_init` parses it (and
  after first-run template generation, when applicable). When 0, the
  file's mode is left alone (see Removed for the corresponding drop
  of the v1.1.0 permissive-mode warning).
- **`config_get_path(cfg)`** — accessor returning the resolved YAML
  config file path the handle is bound to. Used internally by
  AppGuard to drive the `config_mode` chmod, but available to any
  caller that needs to operate on the config file directly (e.g.,
  reload-on-SIGHUP patterns that want to stat the file).

### Behavior

- Both fields default to `0` (additive change — existing callers
  retain v1.1.0 behavior exactly).
- Chmod failures during init are **hard errors**: `appguard_init`
  tears down the partially-built guard and returns NULL. Rationale:
  setting `db_mode` or `config_mode` is a contract assertion ("the
  file MUST be at this mode"), and silently falling back to a more
  permissive mode would undercut that contract.

### Removed

- **Permissive-mode config file warning (1.1.0)** — `config_init` no
  longer stats the config file and emits a stderr warning when it's
  group- or world-readable. The warning second-guessed a choice the
  application author has implicitly made by not setting
  `config_mode`: if they wanted enforcement, they would have asked
  for it. With `config_mode` available as the explicit "I want this
  mode" expression, the warning was redundant noise — and worse,
  fired before the chmod when an app set `config_mode` against a
  pre-existing permissive file, which was confusing UX. The
  underlying enforcement primitive `db_open_with_mode` (1.1.0) is
  unaffected; only the stderr nag is gone.

### Public API / ABI

- No public-header function signatures changed.
- New fields appended to the end of `appguard_config_t`:
  `db_mode`, `config_mode` (both `mode_t`).
- New function added to `<cutils/config.h>`: `config_get_path`.
- `appguard.h` now includes `<sys/types.h>` for `mode_t`.

## [1.1.0] - 2026-05-13

Performance-aware minor release. One new public API for streaming
query results, one new piece of infrastructure (microbenchmark
harness), and a `config_get_str` that is 28× faster than 1.0.3 on
realistic key counts. No public-header function signatures change;
all additions are appended.

### Added

- **`db_iter_*`** — streaming query iterator paired with the
  existing `db_execute`. `db_iter_begin` / `db_iter_next` /
  `db_iter_end` plus the `CUTILS_AUTO_DB_ITER` cleanup macro deliver
  rows one-at-a-time without materializing the full result set: O(1)
  heap regardless of row count, and ~31% faster than `db_execute` on
  the head-to-head `db_iter_1000_rows` benchmark. The API shape is
  symmetric with `cutils_json_iter_t` — stack-allocate the loop,
  `_begin` / `_next` / `_end`, `AUTO_*` for scope-exit cleanup. Use
  the iterator for SELECTs over unknown or large row counts; keep
  `db_execute` for known-small queries where an indexable array is
  ergonomic. Documented in `include/cutils/db.h` with the
  lifetime-ordering rule (iterator must end before `db_close` on
  the same connection — let `CUTILS_AUTO_DB_ITER` fire in a nested
  block, or call `db_iter_end` explicitly).
- **`db_open_with_mode`** — additive sibling to `db_open` that takes
  a `mode_t` and unconditionally `chmod`s the database file to the
  requested mode. Idempotent: the file has the requested mode after
  the call returns, regardless of whether it was created during the
  open or already existed. Intended for daemons whose DB holds
  sensitive data (credentials, audit logs) and that don't want to
  rely on the calling process's umask. WAL/SHM sidecar files are
  not chmod'd by this call — set `umask(0077)` before opening if
  strict perms on those matter.
- **Config file permission warning** — `config_init` now stats the
  parsed config file and emits a stderr warning if it's group- or
  world-readable. c-utils config files commonly hold credentials,
  so a permissive mode is worth flagging at startup. The warning
  is non-fatal — the consuming app decides whether to act on it.
- **Microbenchmark harness** — new `bench/` directory and
  `make bench` target. Auto-calibrating, release-built, dedicated
  build tree, CSV output at `build-bench/results.csv` for before/
  after diffing across optimization branches. Six initial
  benchmarks cover the library's known hot paths. See
  `bench/README.md` for methodology, baseline numbers, and the
  recipe for adding new benchmarks.

### Changed

- **`config_get_str` is ~28× faster** on realistic key counts.
  Two stacked optimizations:
  - **`config.c`** — registered keys are indexed in a parallel
    sorted-by-string array built at the tail of each registration
    phase (`config_init` and `config_attach_db`). Lookups go
    through `bsearch` (log₂ N strcmps) instead of a linear scan.
    Drove the read from ~1889 ns to ~1229 ns on the
    `bench_config_get_str` 300-key fixture.
  - **`config_yaml.c`** — `doc->entries` is sorted by
    (section, key) at the end of `yaml_parse_file`; `yaml_get`
    and `yaml_set`'s lookup go through `bsearch` via a new
    `yaml_find_entry` helper. Drove the remaining cost from
    ~1229 ns to ~67 ns.
  Behavior is unchanged. `yaml_set` only updates an existing
  entry's value in place and never adds new entries, so the sort
  invariant is stable across the doc's lifetime.

### Investigated, rejected

Two optimization attempts didn't clear the cycle's merge bar
(≥5% improvement on one named benchmark, no regression on any
other) and were not merged. Documented here so the findings
aren't lost:

- **`db_execute` strpool** — tried replacing 3000 per-cell
  `strdup` calls with a single growing arena + offset-table +
  end-of-loop fixup. Result: **7% regression** on
  `bench_db_execute_1000_rows`, because the bench's small
  (~10 byte) cells hit glibc's small-bin allocator fast-path,
  which is cheaper than maintaining the arena's growth +
  bookkeeping. The optimization would likely win for cells
  ≥100 bytes; on the common small-cell case it loses. Callers
  who need O(1) heap should use the new `db_iter_*` API.
- **`log_write_db_on` flexible-array-member combine** — tried
  replacing the two allocations in `enqueue_entry` (struct
  calloc + message strdup) with a single malloc using a flex-
  array member at the end of `log_entry_t`. Result: **0.7%
  improvement** — below the bar. The hot-path cost is dominated
  by synchronous `fprintf` to stdout, the queue mutex, and the
  writer-thread `cond_signal` — kernel-side costs that can't be
  optimized at this layer.

### Performance summary

| Benchmark | 1.0.3 | 1.1.0 | Delta |
|---|---:|---:|---:|
| `config_get_str` (300-key registry) | 1889 ns | 67 ns | **−96.5%** (28× faster) |
| `db_iter_1000_rows` (new API) | — | 138 µs | **31% faster than `db_execute`** on the same fixture |
| Other hot paths (db_execute, log_write_*, json_parse_walk, push_build_postfields) | unchanged within bench noise |

See `bench/README.md` for the full table, methodology, and the
hot-path-by-hot-path status (every top-5 hot path is either
optimized-with-numbers or documented as already-optimal-per-bench).

### Public API / ABI

- **No public-header function signatures changed.**
- New types and functions added to `<cutils/db.h>`:
  `cutils_db_iter_t`, `db_iter_begin`, `db_iter_next`,
  `db_iter_ncols`, `db_iter_col_name`, `db_iter_error`,
  `db_iter_end`, `db_iter_end_p`, `CUTILS_AUTO_DB_ITER`,
  `db_open_with_mode`.
- `db_result_t` layout unchanged.

## [1.0.3] - 2026-05-13

Round of post-1.0.2 polish. Three internal fixes, no public API or ABI
changes — public-header function signatures are unchanged, and the
only behavioral shift is documented under **Changed**.

### Fixed

- **CI** — `gcovr` was rejecting the test pipeline on GCC's BUG 68080
  ("negative branch counts in exception-handler regions"), which surfaces
  intermittently on Debian-trixie builds. `gcovr` now tolerates the
  negative-count rows via `--exclude-throw-branches`; coverage numbers
  are unchanged, the gate just stops false-positiving on toolchain bugs.
- **Push** — `send_one` formatted the URL-encoded Pushover POST body
  into a fixed 4KB stack buffer. With `MAX_MSG_CHARS = 1024` and
  `MAX_TITLE_CHARS = 250` the worst-case unencoded total fit, but
  URL-encoding expands each UTF-8 byte up to 3x — a max-length message
  of mostly multi-byte characters could approach the bound, and a
  future bump to either cap would silently truncate. The body is now
  built in `cutils_push_build_postfields` via two-pass `snprintf` into
  a buffer sized exactly to the formatted content. NOMEM yields
  `SEND_TRANSIENT_FAIL` so the row retries normally.

### Changed

- **Config** — env-var overrides are now captured **at registration
  time** (during `config_init` for file keys, during
  `config_attach_db` for DB keys) rather than re-read via `getenv` on
  every `config_get_str` call. This removes the per-read syscall plus
  the per-read `make_env_name` allocation, and closes the POSIX hazard
  where a concurrent `setenv` on any thread invalidates a prior
  `getenv` return pointer. The existing "no `setenv` after
  `appguard_init` returns" guidance in the config header is now
  enforced by behavior rather than externalized as a caller invariant:
  late `setenv` is silently ignored, matching the documented contract.
  Apps that already follow 12-factor practice (set env once at
  startup) are unaffected.

### Documentation

- Removed an orphaned "see task #6" reference from the config-subsystem
  docstring in `include/cutils/config.h`.

### Dead-code sweep

Ran `cppcheck --enable=unusedFunction`, link-time `--gc-sections`
inspection of every exported text symbol (210 total across the
library), and manual reference counts on every public macro, error
code, and private file-scope helper. Result: no removable internal
code. The strict-warning compiler regime (`-Wunused-function`,
`-Wunused-variable`, `-Wunused-parameter`, `-Wunused-result`) plus
broad test coverage have already kept the codebase clean. Public-API
constants that are not consumed internally (`CUTILS_NONNULL`,
several `PUSH_PRIORITY_*` levels) are intentionally retained — they
are documented public surface that consumers may use.

## [1.0.2] - 2026-05-08

Push subsystem durability and disable contract. Two related fixes.
Public function signatures unchanged; one behavioral shift documented
under **Changed**.

### Fixed

- **Push** — the inline retry in `send_one` (1s, 2s, 4s) capped a
  transient outage at a 7-second total budget. Anything longer
  than that — a router restart, an ISP blip, a brief cloud
  incident — silently swallowed the message because the worker
  would `break` after retries exhausted, then park on `cond_wait`
  with no further retry path until a fresh `push_send` happened
  to wake it. Retry now lives in the worker, persisted via two
  new columns (`attempts`, `next_retry_at`) added by migration
  `005_push_retry`. Backoff schedule: 30s, 1m, 2m, 5m, 15m, 1h
  (final entry capped). The worker uses `pthread_cond_timedwait`
  to self-wake at the earliest `next_retry_at`, so retries
  continue across the full lifetime of every queued message
  without depending on caller traffic. Per-message TTL (default
  24h, overridable via `push_opts_t.ttl`) is the give-up clock —
  rows whose `timestamp + ttl` has elapsed are marked `failed=1`
  without a send attempt.
- **Push** — `push_init` left `push_db` set on the
  creds-missing branch, so `push_send` happily inserted rows
  into a queue that nothing drained: a slow leak proportional
  to send rate. `push_db`/`push_cfg` are now nulled on that
  path, matching the `enable_pushover=0` shape and triggering
  the disabled contract below.
- **Push** — `sleep(N)` inside the inline retry blocked
  `push_shutdown` for up to 4 seconds. The new
  `cond_timedwait` design makes shutdown responsive even when
  the next retry deadline is hours away — the shutdown signal
  wakes the worker immediately.
- **Push** — a single transient-failing row could park its
  queued siblings behind it. The worker `break`d the inner
  drain loop on every transient failure, so subsequent rows
  waited for the next external wake-up. The drain now advances
  past a row whose `next_retry_at` has been pushed forward and
  processes anything else that's currently due.

### Changed

- **Push (disabled contract)** — `push_send` and
  `push_send_opts` return `CUTILS_OK` and silently drop the
  message when the subsystem is disabled (any of: AppGuard
  configured with `enable_pushover=0`; Pushover credentials
  not configured; `push_shutdown` already ran). Previously
  `push_send_opts` returned `CUTILS_ERR_INVALID` for the
  not-initialized and post-shutdown cases, forcing every app
  call site to gate on whether push was active. Apps now
  sprinkle `push_send(...)` calls without conditionals and the
  enable decision lives in one place — the AppGuard config.
  Note: this is a behavior change for callers that explicitly
  checked the old error return; the function signatures are
  unchanged.

### Tests

- TTL expiry: rows past `timestamp + ttl` get marked `failed=1`
  on the worker's drain pass, no network call.
- Shutdown responsiveness: with a row deferred 24h into the
  future, `push_shutdown` returns in under 2s (in practice,
  sub-millisecond — the cond signal wakes the timedwait).
- Disabled-contract no-ops: pre-init, missing creds, and
  post-shutdown all return `CUTILS_OK` and leave the push
  table empty.
- Pre-1.0.2 tests that asserted error returns from the
  disabled paths (`test_push_send_not_initialized`,
  `test_push_send_no_creds`, `test_push_send_after_shutdown_rejected`)
  were updated/renamed to assert the new no-op contract.
- `test_concurrent_push_send_multi_part_atomic` reworked to
  use a creds fixture; the pre-fix test relied on no-creds
  leaving `push_db` set so the worker never started, which
  the disabled-contract change closed off.

## [1.0.1] - 2026-04-30

Thread-safety pass. The 1.0.0 README advertises "safe to call from any
thread" semantics across every subsystem; an audit after release found
eleven gaps. All public APIs and ABIs are unchanged — this is a pure
correctness release.

### Fixed

- **DB** — `cutils_db_tx_begin` released the connection mutex after
  `BEGIN`, so a parallel writer's `INSERT` could land inside the tx
  owner's atomic unit and be rolled back along with it. The mutex is
  now `PTHREAD_MUTEX_RECURSIVE` and held across the entire scope from
  `tx_begin` through `tx_commit` / `tx_end_p`.
- **Error loop** — `error_loop_t` had no synchronization. A shared
  detector across threads tore the strdup/free pair on `last_raw`
  (UAF), miscounted, and let the cooldown gate flip
  non-deterministically. Internal mutex added; the threshold
  callback fires outside the lock to avoid re-entry deadlock.
- **Config** — the read API (`config_get_str`) mutated the DB-value
  cache linked list under a `const cutils_config_t *`. Concurrent
  readers raced on list prepend (lost slots, leaks) and on the
  cache slot's `value` pointer (UAF on previously-returned strings).
  All entry points now serialize on a recursive mutex.
- **AppGuard** — the SIGINT/SIGTERM handler called `fprintf`,
  `pthread_mutex_lock`, `pthread_join`, `free`, and `sqlite3_close`,
  none of which are async-signal-safe (`pthread_mutex_lock` from a
  signal handler is explicit UB). Replaced with the standard
  sigwait pattern: `appguard_init` masks the signals process-wide,
  a dedicated `signal_watcher` thread receives them, drains the
  push and log queues, then `_exit(0)`s from a regular thread
  context. SIGUSR1 wakes the watcher cleanly during normal
  `appguard_shutdown`.
- **Log, Push** — globals read across threads (`log_min_level`,
  `log_systemd_mode`, `log_running`, `log_db`, `push_running`,
  `push_db`, `push_cfg`) were plain `int` / pointer; per C11 that's
  a data race. Marked `_Atomic`. Db-handle pointers are now also
  cleared to `NULL` on shutdown so the late-call gate is fully
  closed: `log_write` skips the enqueue, `push_send_opts` returns
  `CUTILS_ERR_INVALID`.
- **Log** — `fire_streams` held the registry mutex across the user
  callback; a callback that re-entered `log_*` self-deadlocked,
  and a slow callback blocked all producers. Snapshot-then-fire
  unlocked. The trade-off (callback may fire briefly after
  `log_stream_unregister` returns) is documented in `cutils/log.h`.
- **Push** — concurrent `push_send` calls on multi-part messages
  produced colliding `timestamp + part` ordering keys, so
  receiver-side reassembly interleaved chunks across senders.
  `split_and_store` now wraps all chunks in a `cutils_db_tx_t`
  (the tx fix above keeps the connection mutex held across the
  whole insert), and the worker drains `ORDER BY rowid` for
  strict FIFO insertion order.
- **Push** — `push_init` set `push_notify = 0` then started the
  worker, so it parked on `cond_wait` immediately and never drained
  rows left over from a previous (possibly crashed) run, despite
  the header advertising exactly that recovery behavior. The
  initial notify is now `1` so the first iteration runs the drain
  loop.
- **Log** — `log_init` did not actually clear `log_systemd_mode`
  despite the comment claiming it did, so an init/shutdown/init
  cycle inherited the prior mode. Reset on every init.

### Documented

- **Config** — `config_get_str` now spells out the thread-safety
  contract: read API is concurrent-safe; the returned `const char *`
  is invalidated by a concurrent `config_set` on the same key;
  apps must not call `setenv` after `appguard_init` (POSIX
  invalidates `getenv`'s return on a cross-thread `setenv`).

### Tests

- Each fix lands with a regression test that fails on the pre-fix
  code: tx-mutex race, error-loop reporter race, config DB-cache
  race, SIGTERM-clean-exit, post-shutdown push reject, callback
  re-entry deadlock, multi-part chunk atomicity, drain-on-startup,
  systemd-mode reinit reset.

## [1.0.0] - 2026-04-30

First formal release. Marks the framework as production-ready and commits
to SemVer at the public-header level (see README "API stability").

### Subsystems shipped

- **Error** (`cutils/error.h`) — thread-local error buffer with enum return
  codes, deepest-frame-sets-message convention.
- **DB** (`cutils/db.h`) — mutex-protected SQLite connection in WAL mode,
  migration runner with SHA256 checksum tracking and savepoint-per-migration
  rollback.
- **Config** (`cutils/config.h`) — two-store config (YAML file + SQLite),
  `ENV_VAR > store > default` precedence, automatic template generation on
  first run.
- **Log** (`cutils/log.h`) — async dual-output logging (console + SQLite),
  TTY-aware ANSI color, optional systemd-native mode with RFC 5424 priority
  prefixes, configurable retention, multi-callback stream API.
- **Push** (`cutils/push.h`) — background Pushover notification worker with
  exponential-backoff retry, message splitting, builder API.
- **Error Loop** (`cutils/error_loop.h`) — repeating-error detector with
  message normalization (strips hex, UUIDs, timestamps).
- **AppGuard** (`cutils/appguard.h`) — lifecycle orchestrator, init/shutdown
  ordering, signal handlers.
- **Memory helpers** (`cutils/mem.h`) — `__attribute__((cleanup))`-based
  scoped cleanup macros (`CUTILS_AUTOFREE`, `CUTILS_AUTOCLOSE`,
  `CUTILS_AUTOCLOSE_FD`, `CUTILS_LOCK_GUARD`, `CUTILS_MOVE`).
- **JSON** (`cutils/json.h`) — vendored cJSON wrapper.
- **Version** (`cutils/version.h`) — `cutils_version()` returns the build
  string baked at compile time (`<semver>_<YYYYMMDD>.<HHMM>.<type>`).

### Build & quality

- C11, strict warnings, hardening flags.
- Build variants: release / debug / asan / coverage; `BUILD_TYPE` reflected
  in `cutils_version()`.
- 289 cmocka tests, 93%+ line coverage.
- Static analysis (stack-usage, gcc `-fanalyzer`, cppcheck, clang-tidy)
  enforced in CI.

### CI

- GitLab pipeline: test (JUnit) → analyze + coverage (cobertura,
  `--fail-under-line 80`) → release.
- `release:auto-tag` creates `vX.Y.Z` on `release_version` bump on master.
- `release:stable` creates the GitLab Release with notes pulled from this
  CHANGELOG.
