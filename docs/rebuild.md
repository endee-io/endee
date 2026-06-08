# Index Rebuild

Rebuild reconstructs an index's HNSW graph with new build parameters (`M`,
`ef_construction`) **without re-uploading any vectors**. All vectors are re-indexed from the
vectors already in MDBX storage; only the graph structure is rebuilt.

It is implemented as a thin composition of mechanisms that already exist in the server:
building a graph from stored vectors (as crash recovery does), persisting it with an atomic
rename (as a normal save does), and hot-swapping the in-memory graph (as a reload does). The
orchestration mirrors the async backup feature.

## Architecture

```
IndexManager
  └── Rebuild rebuild_                 (src/core/rebuild.{hpp,cpp})
        ├── start()                    validate + launch one std::jthread per user
        ├── run()                      the worker: build → persist → swap (holds the write lock)
        └── status()/isActive()/joinAll()
```

`IndexManager` owns a `Rebuild` member and forwards `createRebuildAsync()` /
`getRebuildStatus()` to it. The worker holds the per-index write lock for the whole build, so
upserts/deletes simply wait — the same way they wait during a backup.

## API endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/v1/index/{name}/rebuild` | Start an async rebuild |
| GET | `/api/v1/index/{name}/rebuild/status` | Check rebuild progress |

## Start a rebuild

**POST** `/api/v1/index/{name}/rebuild`

Both parameters are optional; an omitted parameter keeps its current value. At least one must
actually change.

```json
{
    "M": 32,
    "ef_con": 256
}
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `M` | int | HNSW graph connectivity (`MIN_M`–`MAX_M`) |
| `ef_con` | int | Construction-time search width (`MIN_EF_CONSTRUCT`–`MAX_EF_CONSTRUCT`) |

**Response `202 Accepted`:**

```json
{
    "status": "in_progress",
    "previous_config": { "M": 16, "ef_con": 128 },
    "new_config": { "M": 32, "ef_con": 256 },
    "total_vectors": 50000
}
```

**Errors:**

| Code | Condition |
|------|-----------|
| 400 | Invalid JSON, no change specified, M/ef out of range, or an immutable field was sent (`space_type`, `precision`, `dim`, `sparse_model`) |
| 400 | A rebuild or backup is already in progress for this user |
| 404 | Index not found |
| 409 | Index is blocked by an incomplete layout migration |

## Check progress

**GET** `/api/v1/index/{name}/rebuild/status`

Status is tracked per user and persists until the next rebuild starts.

> **Note:** `{name}` is currently informational. Because a user has at most one rebuild at a
> time, status is per-user: any `{name}` returns that user's current/last rebuild, and the
> `index_id` field in the response identifies which index it targets. The path segment is
> reserved to select a specific index's rebuild once multiple rebuilds per user are supported.

| Status | Meaning |
|--------|---------|
| `idle` | No rebuild has run for this user |
| `in_progress` | A rebuild is currently running |
| `completed` | The last rebuild finished successfully |
| `failed` | The last rebuild failed (see `error`) |

```json
{
    "status": "in_progress",
    "index_id": "alice/catalog",
    "vectors_processed": 45000,
    "total_vectors": 100000,
    "percent_complete": 45.0,
    "previous_config": { "M": 16, "ef_con": 128 },
    "new_config": { "M": 32, "ef_con": 256 },
    "started_at": 1717545000
}
```

`completed_at` is added when the rebuild ends; `error` is added when it fails.

## Restrictions

- **Only `M` and `ef_con` can change.** `space_type`, `precision` (quantization), `dim` and
  `sparse_model` are immutable — changing them would require rewriting the stored vector bytes
  (a separate, larger operation) and is rejected with 400.
- **One rebuild per user**, and a rebuild **cannot run concurrently with a backup** for the
  same user (each rejects the other).

## Behaviour

- **All vectors are re-indexed** from MDBX storage into a fresh HNSW graph with the new
  parameters; deleted vectors are not resurrected (they were physically removed from storage).
- **Search continues** during a rebuild — queries run against the old graph until the new one
  is swapped in.
- **Upserts/deletes block** for the duration of the rebuild (the worker holds the per-index
  write lock), the same behaviour as during a backup.
- **Crash safety.** The new graph is written to `default.idx.rebuild` and then atomically
  renamed onto `default.idx` — a single commit point. A crash before the rename leaves the old
  graph and parameters fully intact; the orphan temp file is removed the next time the index
  loads. A crash after the rename leaves the new graph live (the parameters are stored inside
  `default.idx` itself). The index is never left unloadable, so no manual recovery is needed.

## Capacity and timing

- **Disk:** plan for roughly **1× the index file size** of extra space for the temporary
  `default.idx.rebuild` written before the rename.
- **Memory:** the old and new graphs are both in RAM during the build (~**2× the graph size**)
  in addition to normal vector storage.
- **Duration:** dominated by re-inserting every vector into the new graph (≈ a full re-index);
  higher `M`/`ef_con` increases build time.
