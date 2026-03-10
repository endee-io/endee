# Logs

## Format

All production logs must go through `LOG_INFO`, `LOG_WARN`, or `LOG_ERROR` from [src/utils/log.hpp](../src/utils/log.hpp).

The emitted format is:

```text
LEVEL_code: username/index_name: message
LEVEL: username/index_name: message
```

- `LEVEL` is one of `INFO`, `WARN`, or `ERROR`.
- `code` is an explicit numeric message code when present.
- `username/index_name` identifies the user and index tied to the event.
- If a log is not tied to a specific index, placeholders are used:
  - global log: `-/-`
  - user-only log: `username/-`

Examples:

```text
INFO_2023: alice/catalog: Starting reload
WARN_1035: alice/catalog: Invalid k: 0
ERROR_1502: alice/catalog: Failed to store metadata: MDBX_MAP_FULL
INFO: -/-: Starting the server
WARN_1064: alice/-: Backup-info request for missing backup nightly
INFO_1703: -/-: NEON is supported and usable
```

## How To Call Logs

The same macros are used everywhere. The implementation decides how to build context from the arguments:

```cpp
LOG_INFO("Starting the server");
LOG_WARN(1058, username, "Backup download requested for missing backup " << backup_name);
LOG_ERROR(2039, index_id, "Search failed: " << e.what());
LOG_INFO(2045, username, index_name, "Restored backup from " << backup_tar);
```

Supported forms:

```cpp
LOG_INFO(message)
LOG_INFO(code, message)
LOG_INFO(code, context, message)
LOG_INFO(code, username, index_name, message)
```

The same overload shapes apply to `LOG_WARN` and `LOG_ERROR`.

### Context resolution

- `LOG_*(message)`
  - Global log.
  - Context becomes `-/-`.
  - No numeric code is emitted.
- `LOG_*(code, message)`
  - Global log with explicit code.
- `LOG_*(code, context, message)`
  - If `context` contains `/`, it is treated as `index_id` and split into `username/index_name`.
  - Otherwise it is treated as `username` and becomes `username/-`.
- `LOG_*(code, username, index_name, message)`
  - Uses the explicit username and index name directly.

## Rules

- Explicit numeric codes are preferred for stable operational logs.
- Code-less logs are valid and must never receive synthesized IDs.
- Prefer logging at request boundaries, lifecycle transitions, and rare failure paths.
- Do not add logs in hot loops or per-vector/per-result paths.
- Replace `std::cerr` with `LOG_*` so the output format stays consistent.
- For index-scoped code, prefer passing `index_id` and logging with `LOG_*(code, index_id, ...)`.
- Keep messages short and problem-oriented. Include values that help debug the failure.

## Message Code Guidance

- Reuse the existing local code range in the file you are editing when extending current behavior.
- Keep related code ranges grouped by subsystem when practical:
  - `1000s` request/server logs
  - `1200s` filter logs
  - `1300s` backup store logs
  - `1400s` WAL logs
  - `1500s` metadata logs
  - `1600s` vector storage logs
  - `1700s` CPU compatibility logs
  - `2000s` index manager logs
  - `2100s` HNSW load/cache logs

## Where It Is Implemented

- Macro dispatch and formatting live in [src/utils/log.hpp](../src/utils/log.hpp).
- Request-level validation and 500 logging live in [src/main.cpp](../src/main.cpp).
- Index lifecycle and persistence logs live in [src/core/ndd.hpp](../src/core/ndd.hpp).
