# minimal — a buildable c-utils consumer

A complete, runnable example of a c-utils application. Goes from `main()`
to a fully-initialized app — config parsed, DB opened, migrations run,
background threads spawned — with a single `appguard_init` call. Then it
seeds a small `widgets` table, iterates it with the streaming iterator,
bumps a DB-backed counter, sends a notification (no-op without
credentials), and shuts down cleanly.

## Build & run

```sh
cd examples/minimal
make            # builds c-utils too if libc-utils.a is missing
./minimal       # first run: writes config.yaml template, exits
./minimal       # second run: parses config, executes end-to-end
```

## Files this example creates in its CWD

- `config.yaml` — generated on first run with mode `0600`. Holds the
  two file-backed keys (`app.name`, `app.greeting`). Edit then re-run.
- `app.db` — SQLite database. Holds the `system_migrations` tracking
  table, the c-utils internal tables (`config`, `logs`, `push`), and
  the example's own `widgets` table.

`make clean` removes both files plus the executable, so you can demo
the first-run path repeatedly.

## What to look for

- **Config two-store split.** `app.name` and `app.greeting` live in the
  YAML file; `runtime.greeted_count` lives in the SQLite `config` table
  and increments on every run.
- **Migrations.** `app_migrations[]` defines the `widgets` table; the
  c-utils library migrations create `config` / `logs` / `push` /
  `system_migrations` automatically.
- **Streaming iterator.** `demo_iterate` uses
  `CUTILS_AUTO_DB_ITER` + `db_iter_begin` / `db_iter_next` for an O(1)
  heap walk over the widgets table.
- **Logging.** Console output is colored when stdout is a TTY; under
  systemd it auto-switches to RFC 5424 priority prefixes.
  `app.db`'s `logs` table accumulates every log line.
- **Pushover.** `push_send` enqueues unconditionally; if creds aren't
  set in the config it's a no-op and the worker thread isn't spawned.

## Sibling-repo layout assumption

The default `CUTILS_DIR=../..` assumes:

```
~/git/home/c-utils/
  examples/
    minimal/        ← you are here
```

For a real consuming app living elsewhere, override:

```sh
make CUTILS_DIR=/path/to/c-utils
```
