# MDBX Shared-Env ACID Revamp

## Goal

- Move each index from several independent MDBX environments to one shared MDBX environment with named DBIs.
- A logical write commits or aborts as one unit across vectors, metadata, ID mappings, filters, sparse data, and the operation log.
- Durable source of truth: shared MDBX env + global metadata catalog. HNSW graph files stay outside MDBX; committed `op_log` rows drive idempotent HNSW replay after crashes.

## Shared MDBX Layout

- One shared MDBX environment per index at `<index>/vectors`.
- Wrapped by `SharedIndexEnv` (see below).
- Named DBIs:
  - `default` - quantized vector bytes, integer-keyed by numeric id.
  - `vector_meta` - msgpacked `VectorMeta`, integer-keyed by numeric id.
  - `id_map` - external string id → numeric id, plus ID allocator keys.
  - `filter_schema` - persisted filter field type schema.
  - `category_idx` - category filter bitmaps.
  - `numeric_forward` - numeric `<field>:<id>` → sortable value.
  - `numeric_inverted` - numeric range buckets.
  - `sparse_docs` - raw sparse vectors, integer-keyed by numeric id.
  - `blocked_term_postings` - sparse inverted-index blocks, integer-keyed.
  - `op_log` - HNSW replay log, integer-keyed by sequence.
  - `layout_meta` - storage layout version marker.
- `settings::SHARED_INDEX_MAX_DBS` is 32; the layout reserves 11 DBIs and logs a warning when headroom is tight.

### `SharedIndexEnv` (`src/storage/shared_mdbx.hpp`)

- One class, owns the `MDBX_env*` handle and the env path.
- Sets geometry from `settings::VECTOR_MAP_SIZE_BITS` / `_MAX_BITS` and `settings::SHARED_INDEX_MAX_DBS`.
- Opens with `MDBX_WRITEMAP | MDBX_NORDAHEAD` only - no async sync flags, sticky-thread mode kept on (see "Durability Flags").
- Exposes `get()` to retrieve the raw env for Layer 3 to begin transactions on, plus static helpers `write_layout_version(env, v)` / `read_layout_version(env)` that operate on the `layout_meta` DBI.
- Lives as Layer 1 in the layering model below; Layer 2 primitives never call `mdbx_env_open`.

## Transaction Model

User-facing write paths begin one write transaction on the shared env and thread it through the lower layers:

- **Add/upsert vectors**: allocates IDs, replaces sparse docs/postings, writes vector bytes, metadata, filter schema/category/numeric indexes, and appends `op_log` rows - all one MDBX transaction.
- **Delete by id**: removes ID mappings, filter index rows, vector bytes, metadata, sparse docs/postings, and appends delete `op_log` rows - one MDBX transaction.
- **Filter updates**: remove old filter rows, update metadata, write new filter rows/schema - one MDBX transaction.
- Standalone component APIs still exist for tests, legacy utilities, and isolated component usage, but the index manager routes normal loaded-index writes through the shared transaction path.
- **Filter schema cache is staged**: transactional filter writes build a local schema snapshot and write it through the caller's MDBX txn. The in-memory cache is reloaded only after commit, so an abort cannot publish phantom schema fields.

### Op log (`WriteAheadLog`, `src/storage/wal.hpp`)

- One class, backed by the `op_log` DBI inside the same shared env (no separate WAL file).
- Entry types: `VECTOR_ADD = 1`, `VECTOR_DELETE = 2`, `VECTOR_UPDATE = 3` (see `WALOperationType`).
- `append(txn, ...)` writes through the caller's write txn using `MDBX_APPEND` against monotonic sequence keys, so an append participates in the same atomic commit as the data rows.
- `getEntryCount()` returns the entries appended since last clear; `hasEntries()` is the recovery probe.
- `clear(txn)` empties the DBI inside the caller's txn - typically called after `saveIndexInternal` flushes HNSW to disk.
- Recovery replays entries in sequence order and is idempotent (see "HNSW Recovery").

## Durability Flags

### `MDBX_MAPASYNC` - removed

- Production envs do **not** set `MDBX_MAPASYNC`. It was present on master for every dense env and was removed on this branch.
- It's literally the same bit as `MDBX_SAFE_NOSYNC`: the MDBX header defines `MDBX_MAPASYNC = MDBX_SAFE_NOSYNC` at `third_party/mdbx/mdbx.h:1425` and marks `MDBX_MAPASYNC` deprecated. The legacy name describes the *effect* under `MDBX_WRITEMAP` (which we use everywhere): MDBX does asynchronous mmap-flushes instead of synchronous `fdatasync` at commit time. From `third_party/mdbx/mdbx.h:1369-1425`:

> With `MDBX_WRITEMAP` the `MDBX_SAFE_NOSYNC` instructs MDBX to use asynchronous mmap-flushes to disk. [...] MDBX itself just notify operating system that it would be nice to write data to disk, but no more.
>
> [...] a system crash can't corrupt the database, but you will lose the last transactions; because MDBX will rollback to last steady commit since it kept explicitly.

And the commit-flush behaviour at `third_party/mdbx/mdbx.h:3021-3024`:

> MDBX always flushes the OS buffers upon commit as well, unless the environment was opened with `MDBX_SAFE_NOSYNC` or in part `MDBX_NOMETASYNC`.

- MDBX distinguishes **steady commits** (durably synced) from **unsteady commits** (committed in the MDBX sense, not yet synced). With these flags, every `mdbx_txn_commit` returns `MDBX_SUCCESS` after an unsteady commit; on crash MDBX rolls back to the last steady commit, dropping every unsteady commit since. Durability (the **D** in ACID) is traded for throughput.
- **Why the shared-env model can't tolerate it**:
  - **Lost acknowledged writes**: a power loss / kernel panic can roll back the unsteady commit that wrote the data rows + `op_log` entry. The API has already returned success; on restart both are gone.
  - **HNSW / MDBX divergence**: HNSW lives outside MDBX. The write path commits MDBX, runs `addPoint` on HNSW worker threads, then occasionally saves HNSW (gated by `wal->getEntryCount() >= persistence_config_.save_every_n_updates` in the post-commit tail of `addVectors`, `deleteByNumericIds`, and `updateFilters` in `src/core/ndd.hpp`). If a save lands *after* an unsteady commit's `addPoint` but *before* a crash, the HNSW file has the new label but MDBX has rolled back the matching vector/meta row and the `op_log` entry. Search returns labels that fail to populate.
- Master also ran with `MDBX_MAPASYNC` and inherited the lost-acknowledged-writes risk; it just had no shared single-txn recovery contract for the flag to additionally break.
- **Convention**: do not add `MDBX_MAPASYNC`, `MDBX_SAFE_NOSYNC`, `MDBX_NOMETASYNC`, or `MDBX_UTTERLY_NOSYNC` to any production env in this codebase - they all weaken the durability promise that `op_log` recovery depends on.

### `MDBX_NOSTICKYTHREADS` - removed

- No production MDBX env in this codebase uses it. Every env (shared, standalone compatibility, global metadata, sparse, migration source) runs in default sticky-thread mode.
- It was tried during the revamp on the assumption it was needed because shared-index write transactions pass across storage objects. That was a misread of the MDBX docs (`third_party/mdbx/mdbx.h` lines 1176-1204): write transactions must commit on the OS thread that began them **regardless of `MDBX_NOSTICKYTHREADS`**. The flag only relaxes the per-API thread-affinity check; it does not enable cross-thread commit.
- Every shared-env write path (`addVectors`, `deleteByNumericIds`, `updateFilters` in `src/core/ndd.hpp`) is begin → use → commit on one OS thread. "Passed across storage objects" means across C++ objects on one thread, not across OS threads.
- The flag has a significant read-concurrency penalty: under it, reader transactions stop using TLS and every `mdbx_txn_begin(RDONLY)` re-acquires a reader-table slot under lock. The hot search path runs ~k `get_meta` reads per request; at concurrency=16, k=30 this is hundreds of slot acquisitions per round on a single bookkeeping structure.
- Measured impact: in VectorDBBench Performance768D1M / k=30, enabling `MDBX_NOSTICKYTHREADS` collapsed concurrency=16 QPS from ~1716 to ~490 (~71% regression). Removing it restored throughput.
- **Convention**: do not add `MDBX_NOSTICKYTHREADS` to any MDBX env unless a transaction genuinely crosses OS threads between begin and end. No code path does this today.

### Summary

- No unsafe sync flags (`MDBX_SAFE_NOSYNC`, `MDBX_NOMETASYNC`, `MDBX_MAPASYNC`, `MDBX_UTTERLY_NOSYNC`) are enabled.
- No `MDBX_NOSTICKYTHREADS` is enabled.
- Both classes look like free wins from outside; both actively break invariants this branch depends on.

## Layout Versioning

- `IndexMetadata.layout_version` is the catalog-side version. The same version is also stored in the shared env's `layout_meta` DBI.
- Normal load/search/write rejects missing or stale versions as legacy:

```text
Index was created with an old storage layout. Create a backup and restore it to migrate.
```

- Forward-incompatible versions are rejected separately:

```text
Index was created with a newer storage layout. Upgrade this binary before opening it.
```

- The split matters: old layouts are migration sources, newer layouts are not safe to rewrite with this binary.

## Backup and Restore

- The server can still create raw backups of legacy-layout indexes - backups record the source `layout_version`.
- The server's `restoreBackup` only accepts current-layout backups. Legacy backups are rejected with an explicit pointer:

```text
Run `ndd-migrate-v0-to-v2 from-backup --backup <tar> --out-dir <dir>`
(or `ndd-migrate-v0-to-v2 in-place` against a live index folder) before restoring.
```

- All legacy migration logic lives in the offline `ndd-migrate-v0-to-v2` binary (`src/tools/`). See [docs/migrator.md](migrator.md) for the operator surface (subcommands, `--replace-original` flow, marker semantics).
- Restore now, in order:
  1. **Scan-first version check.** `BackupStore::readMetadataJsonFromTar(backup_tar)` walks tar headers and skips payload, parses `metadata.json` in place, and rejects upfront on `layout_version > INDEX_LAYOUT_VERSION` (newer-than-binary) or `!= INDEX_LAYOUT_VERSION` (legacy, with the migrator pointer). A v0 backup tar can be tens of GB; the old order (extract → read metadata → reject) wrote the whole payload to disk just to throw it away.
  2. Extract the backup tar into `<user_temp>/<backup_name>`.
  3. Re-read `metadata.json` from the extracted folder and re-apply the same version checks (defensive, in case the scan-first parse failed silently).
  4. Copy the extracted folder into `target_temp_dir`.
  5. Open `target_temp_dir/vectors/` as a `SharedIndexEnv` and validate `layout_meta` matches `INDEX_LAYOUT_VERSION`.
  6. Rename `target_temp_dir` → `target_dir`, write the catalog row with the current layout version, call `loadIndex`.
- `migration.inprogress` marker (`settings::INDEX_MIGRATION_MARKER`): produced **only** by the offline migrator. Server `loadIndex` / `getIndexInfo` / `VectorStorage` open paths refuse marker-bearing folders and tell the operator to re-run the migrator.

## HNSW Recovery

- HNSW files live outside MDBX. Recovery is driven by `op_log` replay:
  - The MDBX transaction commits vector/meta/filter/sparse rows and the `op_log` entry together.
  - On open, `IndexManager::recoverFromWAL` reads the `op_log` if `WAL::hasEntries()` is true.
  - `VECTOR_ADD` and `VECTOR_UPDATE` replay upsert labels into HNSW.
  - `VECTOR_DELETE` replay marks labels deleted only when the label exists and is not already deleted.
  - Replay is idempotent; after replay the index is saved and `op_log` is cleared inside one write txn.

## Performance Choices

- Named DBIs avoid multiple env opens and let one txn cover related writes.
- Numeric filter batch writes no longer allocate per-chunk vector copies; the chunked writer iterates by range.
- Sparse vector upserts are replacements, so stale postings are removed in the same transaction.
- Filter schema cache is reloaded only when the committed transaction actually changed schema.
- `MDBX_APPEND` is used for monotonic `op_log` sequence keys.
- `MDBX_WRITEMAP` is retained; async map flushing is not.

## What Remains

After the revamp these boundaries still exist:

- **Async sparse leg keeps a separate read txn**. `searchKNN` opens one `MDBX_TXN_RDONLY` on the shared env on the main thread and threads it through filter bitmap, brute-force scoring, result metadata, and optional vector bytes. The sparse leg runs in `std::async` and must begin/use/end its txn on the worker thread (sticky-thread mode). One logical search consumes two read transactions.
- **`getVector` is single-snapshot on the shared layout**. One `MDBX_TXN_RDONLY` covers `id_map`, vector bytes, metadata, and sparse docs.
- **Global metadata is a separate MDBX env** and cannot share a single MDBX transaction with a per-index env. Cross-boundary ops are `createIndex`, `restoreBackup`, `deleteIndex`. `saveIndexInternal` also updates global `total_elements`, but that count is catalog/listing metadata, not the source of truth for committed vectors.
- **HNSW graph files are outside MDBX** by design. `op_log` closes the crash-recovery gap; HNSW itself is not part of the MDBX txn.
- **Standalone component constructors and txn wrappers** remain for tests, migration helpers, and compatibility paths. Normal loaded-index writes use the shared txn path.

## Global Metadata Atomicity

- Normal add, upsert, delete, filter, numeric, sparse writes commit all durable per-index rows + `op_log` inside the shared per-index MDBX txn. They do not need to touch the global catalog.
- Operations that do cross the global catalog/per-index boundary:
  - `createIndex` - creates the per-index shared env + initial files, then stores the catalog row. A failure between can leave an orphan dir or a catalog row pointing at an incomplete index.
  - `restoreBackup` - publishes a restored index dir, then writes the catalog row. A failure between can leave restored data invisible to the catalog, or cleanup work after a metadata failure.
  - `deleteIndex` - deletes the catalog row and tombstones the per-index dir. A failure between leaves catalog and on-disk state disagreeing.
  - `saveIndexInternal` - saves HNSW/sparse side effects and updates global `total_elements`. Weaker than create/restore/delete because per-index rows remain authoritative and the count can be recomputed.
- MDBX cannot atomically commit one transaction across two environments. Closing the remaining gap means either moving the catalog into the same physical MDBX env as the indexed rows, or adding an explicit intent/complete-marker publish protocol.

## Read Snapshot Gap

Reads are less dangerous than writes because every component read txn observes a valid committed snapshot, but separate read txns are not guaranteed to be the *same* snapshot. The write revamp prioritized commits; reads have been tightened on the shared-layout path:

- **`getVector` previously stitched id_map + vector bytes + vector meta + optional sparse docs across separate read txns** - delete+id-reuse between them could mix pieces from different states. Shared-layout `getVector` now opens one `MDBX_TXN_RDONLY` and passes it through `IDMapper::get_id`, `VectorStorage::get_vector`, `VectorStorage::get_meta`, `SparseVectorStorage::get_vector`.
- **`searchKNN` previously stitched ~30+ snapshots** for a k=30 filtered hybrid search (one per category lookup, per numeric range, per `visit_vectors_by_ids`, inside the sparse `InvertedIndex::search`, per result `get_meta`, per result `get_vector` when `include_vectors`, and one per HNSW vector-cache miss). Shared-layout `searchKNN` now:
  - Opens one main-thread `MDBX_TXN_RDONLY` at the top of the function.
  - Threads it through `Filter::computeFilterBitmap`, `VectorStorage::visit_vectors_by_ids`, `VectorStorage::get_meta`, `VectorStorage::get_vector`, and HNSW's `searchKnn`.
  - HNSW takes the txn as a parameter and threads it into `VectorFetcher` / `VectorFetcherBatch` closures (`std::function<bool(MDBX_txn*, idInt, uint8_t*)>` and the batch variant) so every cache-miss read uses the request snapshot.
  - Every MDBX read the main thread issues during one search request sees one snapshot.
- **Why this is also a correctness requirement, not just a snapshot nicety**: MDBX sticky-thread mode allows one `MDBX_TXN_RDONLY` per thread. An earlier draft kept HNSW's fetcher closures wired to a self-managing `VectorStore::get_vector_bytes(buf)` that itself called `mdbx_txn_begin`. The second `mdbx_txn_begin` on the same thread returned `MDBX_BAD_RSLOT`, the fetcher silently returned `false`, HNSW computed distances against uninitialised buffers, and recall on a 1M-vector int16-quant index collapsed from ~97% to ~32% on a cold cache. The current `VectorFetcher` signature accepts an `MDBX_txn*` so the fetcher routes through the caller's snapshot and no nested `mdbx_txn_begin` ever happens on the search thread.
- **The async sparse leg cannot reuse the main-thread transaction** - sticky-thread mode forbids it. The sparse `std::async` lambda opens its own `MDBX_TXN_RDONLY` on its worker thread and passes it as the first arg to `SparseVectorStorage::search` / `InvertedIndex::search`. The two transactions begin within microseconds; they can diverge only if a writer commits in that window - the same cross-snapshot risk that already exists between any concurrent reader and writer.
- **Long-held read txn during HNSW**: holding the read txn through HNSW traversal pins MDBX page reclamation against that snapshot. Under sustained write load with many concurrent long-`ef` searches this can grow `mi_recent_txnid` vs. `mi_latter_reader_txnid` (write amplification). If this becomes measurable, `mdbx_txn_park` / `mdbx_txn_unpark` are an option (park the reader between MDBX hops while keeping its snapshot). Until measurements show the gap matters, the simpler one-transaction-per-thread story stands.

The convention "every read takes an `MDBX_txn*`" is now uniform across primitive storage classes - `VectorStore`, `MetaStore`, `IdMapper`, `WAL`, `CategoryIndex`, `NumericIndex`, `Filter`, `InvertedIndex`, `SparseVectorStorage`. The parallel no-txn API was deleted; the `_txn` suffix was then dropped, so the bare names (`computeFilterBitmap`, `range`, `get_bitmap`, `get_bitmap_by_key`, `visit_vectors_by_ids`, `get_meta`, `get_vector`, `search`, `log`, `clear`, ...) are the txn-taking methods. The outer `VectorStorage` convenience layer keeps three no-txn one-shot wrappers (`get_vector(id)`, `get_meta(id)`, `visit_vectors_by_ids(ids, visitor)`) - they each open an `MDBX_TXN_RDONLY` and delegate to the txn-bearing primitive, for callers that don't own a snapshot: `recoverFromWAL`, the non-shared-env `getVector` fallback, and the HNSW write-path fetcher fallback `VectorStore::get_vector_bytes(id, buf)`.

## Layered Read API: One Transaction, One Convention

### Why the parallel API was deleted

- The original code had two ways to read every value: a `_txn` method taking `MDBX_txn*`, and a no-txn sibling that opened its own.
- That parallel API was the proximate cause of the recall regression above: a caller could open `main_txn`, then unknowingly call a function that opened its *own* transaction, and on a sticky-thread env the second `mdbx_txn_begin` returned `MDBX_BAD_RSLOT` while the called function silently degraded. The bug was invisible at the call site - both signature and doc comment looked fine.
- Three classes of bug become structurally impossible after the parallel API is gone:
  1. **Hidden nested transactions on the same thread.** The recall bug. Now "did this method open a txn?" is answered by the signature, not by reading the implementation.
  2. **Cross-snapshot inconsistency within a logical operation.** Old `searchKNN` could observe category index at one snapshot, numeric at another, `get_meta` at a third. Now every read in one request sees one snapshot.
  3. **"Did this method open a txn?" call-site surprises.** Now always "no - the caller did" at the primitive layer.

### The layers

The transaction lifecycle belongs to exactly one layer per request - the request operation (Layer 3). The "every read takes an `MDBX_txn*`" rule applies strictly to Layer 2 primitives; everything above is thin convenience.

```text
┌──────────────────────────────────────────────────────────────────┐
│  Layer 4 - HTTP boundary                  src/main.cpp           │
│    • Parses request, calls Layer 3, serializes response          │
│    • No MDBX knowledge                                           │
├──────────────────────────────────────────────────────────────────┤
│  Layer 3 - Request operation              src/core/ndd.hpp       │
│    • IndexManager methods: searchKNN, getVector, addVectors,     │
│      deleteByNumericIds, updateFilters, recoverFromWAL, ...      │
│    • OWNS the MDBX_txn lifecycle for one request                 │
│    • Opens at entry, aborts/commits at every exit                │
│    • Threads the txn into every Layer 2 / HNSW call              │
├──────────────────────────────────────────────────────────────────┤
│  Layer 2.5 - HNSW search                  src/hnsw/hnswalg.h     │
│    • In-memory graph. Takes MDBX_txn* on searchKnn entry.        │
│    • Threads it into vector_fetcher_ / vector_fetcher_batch_     │
│      on every node visit.                                        │
│    • Fetcher signature: bool(MDBX_txn*, idInt, uint8_t*)         │
├──────────────────────────────────────────────────────────────────┤
│  Layer 2 - Storage primitives             src/storage/, src/filter/, │
│    • VectorStore, MetaStore, IdMapper, WAL,          src/sparse/ │
│      CategoryIndex, NumericIndex, Filter,                        │
│      InvertedIndex, SparseVectorStorage                          │
│    • Every read/write method's first parameter is MDBX_txn*.     │
│    • Never calls mdbx_txn_begin or mdbx_txn_abort.               │
│    • Pure functions of (txn, args) → result.                     │
├──────────────────────────────────────────────────────────────────┤
│  Layer 1 - MDBX env wrapper               src/storage/shared_mdbx.hpp│
│    • SharedIndexEnv. Owns env handle, geometry, DBI registry.    │
│    • Exposes raw env() for Layer 3 to begin txns on.             │
├──────────────────────────────────────────────────────────────────┤
│  Layer 0 - MDBX primitives                third_party/mdbx       │
└──────────────────────────────────────────────────────────────────┘
```

- **Dependency rule**: each layer depends only on layers below.
- **Ownership rule**: the `MDBX_txn` lifecycle belongs to exactly one layer per request - Layer 3.

### The convenience layer above Layer 2

- `VectorStorage` (the wrapping class combining `vector_store_`, `meta_store_`, `filter_store_`) keeps three no-txn one-shot wrappers - `get_vector(id)`, `get_meta(id)`, `visit_vectors_by_ids(ids, visitor)`.
- Each opens an `MDBX_TXN_RDONLY` on the right env, delegates to the txn-bearing primitive, aborts.
- They exist for callers that genuinely don't own a snapshot: `recoverFromWAL` (runs before request-level txns exist), the non-shared-env `getVector` fallback (separate envs per store means no single covering txn), the HNSW write-path fetcher fallback `VectorStore::get_vector_bytes(id, buf)` (the `addPoint` graph traversal does not have a request txn - insertion *is* the write txn).
- They do not violate the "single read API" goal: the primitive they delegate to is the same one a Layer-3 caller with a txn would call. Pure convenience for legacy callers; new code should always pass its own txn.

### What this convention does not address

- Does not collapse the **async sparse leg** into the main thread's transaction - sticky-thread mode forbids that.
- Does not eliminate the **long-held read txn** during HNSW traversal - `mdbx_txn_park` is the path if it becomes measurable.
- Does not touch the **cross-env atomicity gap** between per-index stores and the global metadata catalog.

## Verification

- `cmake --build build-acid -j4`
- `ctest --test-dir build-acid --output-on-failure`
- Current test run: 62/62 configured tests pass; the existing skipped benchmark/hypothesis cases are unchanged.
