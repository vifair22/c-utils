# c-utils — Shared C Application Framework

## Overview

Static library (`libc-utils.a`) providing a common application core for C projects. Six subsystems: Config, DB, Logging, Pushover, Error Loop Detector, and AppGuard (lifecycle manager).

Modeled after python-utils and rust-utils — same philosophy, C implementation.

**Dependencies:** sqlite3, libcurl, cJSON (vendored)

---

## Error Handling

Standard pattern:
- Enum return codes (OK=0, ERR_*<0)
- Thread-local error buffer with `set_error(code, fmt, ...)`
- Deepest function sets the message, callers propagate the code

## Threading

c-utils manages its own threads internally. Consuming apps should not need to be aware of internal threading. Background threads for log writer and push worker. DB access via mutex-protected connection (SQLite WAL mode + SQLITE_THREADSAFE=2).

---

## Subsystem 1: Config Manager

### Two Stores

- **YAML file**: bootstrap + persistent settings. Hand-rolled parser/writer (c-utils controls the format: simple `section:` -> `key: value`). Line-level read/modify/write preserves comments on mutation.
- **SQLite DB**: runtime-mutable app settings. c-utils owns the table and migrations.

### Key Definition

```c
typedef struct {
    const char *key;           /* dot-notation: "ups.device" */
    config_type_t type;        /* CFG_STRING, CFG_INT, CFG_BOOL */
    const char *default_value; /* "9600" */
    const char *description;   /* "Modbus baud rate" */
    config_store_t store;      /* CFG_STORE_FILE or CFG_STORE_DB */
    int required;              /* 1 = must be non-empty, 0 = empty is valid */
} config_key_t;
```

### Section Definitions (separate from keys)

```c
typedef struct {
    const char *prefix;        /* "ups" */
    const char *display_name;  /* "UPS Configuration" */
} config_section_t;
```

Section association is automatic from the dot prefix.

### Key Categories

1. **c-utils hard minimums** (file-only, immutable via API): `db.path`
2. **c-utils optionals** (log level, Pushover creds): app chooses file or DB store at compile time
3. **App file keys**: registered via static array, mutable via API
4. **App DB keys**: registered via static array, mutable via API

Each key lives in exactly one store — enforced at registration, fatal error on collision.

### Read Precedence

```
ENV_VAR > store value (file or DB) > compiled-in default
```

Env var naming: uppercase dot-notation with underscores, prefixed with app name. `ups.device` -> `AIRIES_UPS_DEVICE`. Prefix inherited from `app_name` set at init.

### Template Generation (First Run)

When config file doesn't exist:
- Generate YAML with all file-stored keys, defaults filled in, per-key description comments, section header comments from section definitions
- App controls behavior via `on_first_run`: `CFG_FIRST_RUN_EXIT` (default) or `CFG_FIRST_RUN_CONTINUE`

### Validation

- On load: all required file keys must be non-empty, bail with clear error if not
- DB keys: always have defaults from schema, no startup validation

### Mutation API

- File keys (non-minimum): read/modify/write YAML, preserving comments via line-level approach
- DB keys: full CRUD via SQL
- No change notifications — app restart required to pick up mutations

### Unified Read API

`config_get_str("section.key")` — consumer doesn't care which store, c-utils resolves it.

---

## Subsystem 2: DB / Migrations

- Mutex-protected SQLite connection, WAL mode
- Public API: `db_execute(query, params)` for reads, `db_execute_non_query(query, params)` for writes
- Migration runner: SHA256 checksum tracking in `system_migrations` table, savepoint-per-migration rollback
- Two-tier migrations:
  - **c-utils internal**: compiled-in as static strings (no .sql files to distribute), tracked with `_lib/` prefix
  - **App migrations**: `.sql` files from app's migrations directory
- c-utils creates its own tables: `config`, `logs`, `push`

---

## Subsystem 3: Logging

- Four levels: debug, info, warning, error
- Dual output: stdout (normal) + stderr (errors), colored console output
- Async SQLite persistence via background writer thread
- Auto-cleanup: configurable retention (app sets `log_retention_days`)
- Log stream: multiple callback registrations via function pointers `void (*on_log)(timestamp, level, func, msg)`. c-utils calls all registered callbacks on every log event (after level filtering).
- Calling-function detection

---

## Subsystem 4: Pushover

- Background worker thread polls `push` table for unsent messages
- Retry with exponential backoff (3 attempts), permanent failure marking
- Message splitting on newline boundaries at 1024 chars
- `push(title, message)` convenience function + builder pattern (TTL, timestamp overrides)
- Starts only if `enable_pushover` is set and creds are configured
- Reads token/user from config keys (app chooses file or DB store)
- Uses libcurl

---

## Subsystem 5: Error Loop Detector

- Standalone utility, not tied to AppGuard lifecycle
- App creates instances with threshold + cooldown + callback
- `error_loop_report(detector, message)` / `error_loop_success(detector)`
- Normalizes messages (strips hex, UUIDs, timestamps) for stable comparison
- App manages its own instances

---

## Subsystem 6: AppGuard (Lifecycle Manager)

### Init Config

```c
appguard_config_t cfg = {
    .app_name = "airies-ups",
    .on_first_run = CFG_FIRST_RUN_EXIT,
    .enable_pushover = true,
    .log_retention_days = 30,
    .file_keys = app_file_keys,
    .db_keys = app_db_keys,
    .sections = app_sections,
};
appguard_init(&cfg, argc, argv);
```

### Init Order

1. Parse config file (or generate template + exit/continue)
2. Validate required file keys
3. Init DB (open connection using `db.path`, WAL mode)
4. Run c-utils internal migrations (compiled-in, `_lib/` prefix)
5. Run app migrations (from .sql files)
6. Seed DB config table with defaults for any new DB-stored keys
7. Start log writer thread
8. Start push worker thread (if enabled + configured)
9. Install signal handlers (SIGINT, SIGTERM)
10. Return to app

### Shutdown Order (reverse)

1. Stop push worker
2. Stop log writer
3. Close DB

---

## Build

- `make` builds `libc-utils.a`
- `make test` runs cmocka tests
- `make analyze` runs static analysis (gcc -fanalyzer, cppcheck)
- `make lint` runs clang-tidy
- Consuming apps: `CUTILS_DIR=../c-utils` in Makefile, `-I$(CUTILS_DIR)/include -L$(CUTILS_DIR) -lc-utils`

### Project Structure

```
c-utils/
├── include/
│   ├── cutils.h              — public API (single include)
│   ├── cutils/
│   │   ├── appguard.h         — lifecycle manager
│   │   ├── config.h           — config manager
│   │   ├── db.h               — database + migrations
│   │   ├── log.h              — logging
│   │   ├── push.h             — pushover notifications
│   │   └── error_loop.h       — error loop detector
├── src/
│   ├── appguard.c
│   ├── config.c
│   ├── config_yaml.c          — YAML parser/writer
│   ├── db.c
│   ├── migrations.c           — migration runner + compiled-in lib migrations
│   ├── log.c
│   ├── push.c
│   └── error_loop.c
├── lib/
│   └── cJSON/                 — vendored
│       ├── cJSON.c
│       └── cJSON.h
├── tests/
│   ├── test_config.c
│   ├── test_db.c
│   ├── test_migrations.c
│   ├── test_log.c
│   ├── test_push.c
│   └── test_error_loop.c
├── Makefile
├── release_version
├── LICENSE                    — GPL
└── PLAN.md
```

### Dependencies

| Dependency | Type | Purpose |
|-----------|------|---------|
| sqlite3 | System library | Embedded database |
| libcurl | System library | Pushover HTTP requests |
| cJSON | Vendored (MIT) | JSON parsing/generation, exported to consumers |
| cmocka | Dev dependency | Unit testing |

### cJSON

Vendored single .c/.h, MIT licensed. Built into `libc-utils.a`. Headers exported to consuming apps — they get JSON for free.

---

## Implementation Order

1. Project skeleton (Makefile, directory structure, release_version, LICENSE)
2. Error handling foundation (return codes, error buffer)
3. DB subsystem (SQLite wrapper, mutex, WAL mode)
4. Migration runner (compiled-in lib migrations + file-based app migrations)
5. Config manager — file store (YAML parser/writer, template generation, validation)
6. Config manager — DB store (table seeded by migrations, unified read API, env var override)
7. Logging (console output, DB persistence thread, stream callbacks, auto-cleanup)
8. Pushover (DB queue, worker thread, retry, message splitting)
9. Error loop detector
10. AppGuard (lifecycle orchestrator, signal handlers, init/shutdown ordering)
11. Tests for all subsystems
12. Static analysis, linting, documentation
