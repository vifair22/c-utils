# c-utils

Shared C application framework. Static library providing config management, SQLite database access, structured logging, Pushover notifications, error loop detection, and lifecycle orchestration. Built for Linux, C11, with minimal dependencies.

## Subsystems

| Module | Header | Purpose |
|--------|--------|---------|
| **Error** | `cutils/error.h` | Thread-local error buffer with enum return codes |
| **DB** | `cutils/db.h` | Mutex-protected SQLite connection (WAL mode), migration runner |
| **Config** | `cutils/config.h` | Two-store config (YAML file + SQLite), env var overrides |
| **Log** | `cutils/log.h` | Async logging to console (TTY-aware color, optional systemd-native mode) + SQLite, configurable retention |
| **Push** | `cutils/push.h` | Background Pushover notification worker with retry |
| **Error Loop** | `cutils/error_loop.h` | Detects repeating errors with message normalization |
| **AppGuard** | `cutils/appguard.h` | Lifecycle manager, orchestrates init/shutdown of all subsystems |

Include `<cutils.h>` for everything, or individual headers for specific subsystems.

## Dependencies

**Build-time:**
- GCC (C11)
- GNU Make

**Link-time:**
- [SQLite3](https://sqlite.org) (`-lsqlite3`)
- [libcurl](https://curl.se/libcurl/) (`-lcurl`)
- [OpenSSL](https://www.openssl.org) libcrypto (`-lcrypto`)
- pthreads (`-lpthread`)

**Vendored (included):**
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON parsing, built into the static library

**Test-time:**
- [cmocka](https://cmocka.org) (`-lcmocka`)

**Analysis (optional):**
- [cppcheck](https://cppcheck.sourceforge.io)
- [clang-tidy](https://clang.llvm.org/extra/clang-tidy/)

## Building

```sh
make              # release build (-O2) -> libc-utils.a
make debug        # debug build (-Og -g)
make asan         # AddressSanitizer build

make test         # run all tests
make test-asan    # run tests under AddressSanitizer

make analyze      # stack-usage check + gcc -fanalyzer + cppcheck
make lint         # clang-tidy
make clean
```

## Linking from a consuming app

Directory layout (sibling repos):

```
~/git/home/
  c-utils/          # this repo
  my-app/           # consuming application
```

In your app's Makefile:

```makefile
CUTILS_DIR = ../c-utils

INCLUDES = -Isrc -I$(CUTILS_DIR)/include -I$(CUTILS_DIR)/lib/cJSON
LIBS     = -L$(CUTILS_DIR) -lc-utils -lsqlite3 -lcurl -lcrypto -lpthread
```

Build c-utils first, then your app:

```sh
cd ../c-utils && make
cd ../my-app && make
```

## Quick start

Minimal application using AppGuard to bootstrap everything:

```c
#include <cutils.h>

/* App-specific config keys (YAML file) */
static const config_key_t file_keys[] = {
    { "app.name", CFG_STRING, "my-app", "Application display name",
      CFG_STORE_FILE, 1 },
    { NULL }  /* sentinel */
};

/* App-specific config sections */
static const config_section_t sections[] = {
    { "app", "Application Settings" },
    { NULL }
};

int main(int argc, char **argv)
{
    appguard_set_argv(argc, argv);

    appguard_config_t cfg = {
        .app_name               = "my-app",
        .on_first_run           = CFG_FIRST_RUN_EXIT,
        .file_keys              = file_keys,
        .sections               = sections,
        .log_level              = LOG_INFO,
        .log_retention_days     = 30,
        .log_systemd_autodetect = 1,  /* journald-aware console output when run under systemd */
    };

    appguard_t *guard = appguard_init(&cfg);
    if (!guard) return 1;

    cutils_config_t *config = appguard_config(guard);
    cutils_db_t *db = appguard_db(guard);

    log_info("started: %s", config_get_str(config, "app.name"));

    /* ... application logic ... */

    appguard_shutdown(guard);
    return 0;
}
```

On first run, AppGuard generates a `config.yaml` template with defaults and descriptions, then exits so the user can review it. Subsequent runs parse the config, open the database, run migrations, and start background threads.

## App migrations

Migrations can be loaded from `.sql` files at runtime or compiled into the binary.

### File-based (runtime)

```c
appguard_config_t cfg = {
    /* ... */
    .migrations_dir = "migrations/",
};
```

Place numbered `.sql` files in the directory:

```
migrations/
  001_users.sql
  002_posts.sql
```

### Compiled-in (no files to distribute)

Generate a C source file from your `.sql` files at build time:

```makefile
src/migrations_compiled.c: $(wildcard migrations/*.sql)
	$(CUTILS_DIR)/tools/embed_sql.sh migrations/ app_migrations > $@
```

Reference the generated array in your init config:

```c
extern const db_migration_t app_migrations[];

appguard_config_t cfg = {
    /* ... */
    .migrations = app_migrations,
};
```

Both can be set simultaneously — compiled migrations run first, then file-based.

## Project structure

```
include/
  cutils.h              umbrella header
  cutils/               per-subsystem public headers
src/                    implementation
lib/cJSON/              vendored JSON library
tests/                  cmocka unit tests
tools/
  embed_sql.sh          SQL-to-C migration embedder
```

## License

GPL-3.0-or-later
