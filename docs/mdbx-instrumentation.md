# MDBX Timing Instrumentation

## Overview

Including a built-in timing instrumentation for libmdbx operations. When enabled, every MDBX API call (transactions, gets, puts, cursor operations, etc.) is timed using monotonic clocks, and cumulative statistics are collected in a thread-safe global table.

## Enabling Instrumentation

Set the `ND_MDBX_INSTRUMENT` CMake option at configure time:

```bash
cmake -DND_MDBX_INSTRUMENT=ON ..
```

This defines the `ND_MDBX_INSTRUMENT` preprocessor macro, which activates the timing code inside `third_party/mdbx/mdbx.c`. When the option is `OFF` (the default), all instrumentation macros compile to no-ops and `print_mdbx_stats()` is an empty function, so there is zero overhead in production builds.

## What Gets Timed

The following MDBX operations are individually tracked:

| Command | Description |
|---|---|
| `mdbx_env_create` | Environment creation |
| `mdbx_env_open` | Environment open |
| `mdbx_env_close` | Environment close |
| `mdbx_txn_begin` | Transaction begin |
| `mdbx_txn_commit` | Transaction commit |
| `mdbx_txn_abort` | Transaction abort |
| `mdbx_dbi_open` | Database handle open |
| `mdbx_get` | Key lookup |
| `mdbx_put` | Key/value write |
| `mdbx_del` | Key delete |
| `mdbx_cursor_open` | Cursor open |
| `mdbx_cursor_close` | Cursor close |
| `mdbx_cursor_get` | Cursor read |
| `mdbx_cursor_put` | Cursor write |
| `mdbx_cursor_del` | Cursor delete |

## Reading the Stats

Call `print_mdbx_stats()` to dump the accumulated statistics to `stderr` and reset all counters. Each line looks like:

```
[MDBX_STATS] mdbx_get count=4821 total_ms=12.345 avg_us=2.561
```

- **count** — number of calls since the last `print_mdbx_stats()` call
- **total_ms** — cumulative wall-clock time in milliseconds
- **avg_us** — average time per call in microseconds

If no MDBX commands were recorded since the last call, it prints:

```
[MDBX_STATS] no recorded commands
```

### Where it's called

Currently the health-check endpoint (`GET /api/v1/health`) calls `print_mdbx_stats()` alongside the other debug stat printers. Hit the health endpoint to flush stats to stderr:

```bash
curl http://localhost:<port>/api/v1/health
```

You can also call `print_mdbx_stats()` from anywhere in the codebase — it is declared in `mdbx.h` and is always safe to call (it's a no-op when instrumentation is disabled).

## How It Works

Each instrumented MDBX function uses two macros:

1. **`MDBX_DEBUG_STATS_SCOPE(cmd)`** — placed at the top of the function; captures the start timestamp via `osal_monotime()`.
2. **`MDBX_DEBUG_STATS_RETURN(cmd, value)`** — used in place of `return`; computes the elapsed time and records it before returning.

The stats are stored in a mutex-protected global table. `print_mdbx_stats()` atomically snapshots and resets the table, so each call shows the delta since the previous call.