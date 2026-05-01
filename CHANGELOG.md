# Changelog

All notable changes to c-utils are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) at
the public-header level (see README "API stability").

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
