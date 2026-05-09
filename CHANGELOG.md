# Changelog

All notable changes to c-utils are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) at
the public-header level (see README "API stability").

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
