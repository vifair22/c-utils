# Changelog

All notable changes to c-utils are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) at
the public-header level (see README "API stability").

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
