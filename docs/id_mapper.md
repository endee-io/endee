# ID Mapper Documentation

## Overview

- `IDMapper` maps external string ids (user-provided) to internal numeric ids used by HNSW.
- Provides atomic id generation, point lookups, and id recycling.
- All public methods take the caller's `MDBX_txn*` as the first parameter — the class never opens its own transactions. See [docs/mdbx_shared_env_acid_revamp.md](mdbx_shared_env_acid_revamp.md) for the layered transaction model.

## Architecture

### Storage backend
- Lives as the `id_map` named DBI inside the per-index shared MDBX env (created by `SharedIndexEnv`, `src/storage/shared_mdbx.hpp`).
- No separate `id_mapper/` directory on disk; sizing is driven by the shared env geometry (`settings::VECTOR_MAP_SIZE_BITS` / `_MAX_BITS`), not a per-component map.
- Bloom filter still lives in a separate file alongside the index: `id_bloom.bin`, for fast negative lookups.

### Key components

#### 1. Core mapping storage
- **Format**: `string_id → idInt (numeric_id)`, stored as direct key-value pairs in the `id_map` DBI.
- **Atomicity**: every write is the caller's MDBX txn, so id allocation, vector bytes, metadata, filter rows, and the `op_log` row commit together.

#### 2. Special Keys
The database contains three special internal keys with random suffixes to avoid collisions:

```cpp
NEXT_ID_KEY = "__next_id_px7b39lw__"       // Stores next available numeric ID
DELETED_IDS_KEY = "__deleted_ids_px7b39lw__" // Stores array of reusable IDs
BLOOM_FILTER_FILENAME = "id_bloom.bin"     // Bloom filter file
```

#### 3. ID Generation Strategy
- **Sequential Generation**: IDs start from 1 and increment
- **Atomic Counter**: `NEXT_ID_KEY` stores the next available ID
- **ID Recycling**: Deleted IDs are stored and reused before generating new ones
- **Batch Generation**: Multiple IDs generated atomically in single transaction

#### 4. Bloom Filter Integration
- **Purpose**: Fast negative lookups to avoid unnecessary MDBX queries
- **False Positive Rate**: 1% (0.01)
- **Auto-sizing**: Grows based on element count using bit-doubling
- **Persistence**: Saved to disk and loaded on startup

## Core Operations

### 1. ID Creation - `create_ids_batch()`

Single method for mapping string ids to numeric ids; runs inside the caller's write txn:

```cpp
std::vector<std::pair<idInt, bool>>
create_ids_batch(MDBX_txn* txn, const std::vector<std::string>& str_ids);
```

Returns `(numeric_id, is_new_to_db)` per input string. `is_new_to_db == true` means the string id was not previously mapped — the caller (e.g. `VectorStorage::store_vectors_batch`) treats this as a fresh insert; `false` means an upsert against an existing id.

**Process flow** (all inside the caller's txn):

1. **Bloom filter check** — quick negative filter; only `might_exist` strings proceed to MDBX.
2. **MDBX lookup** via the caller's txn — returns existing numeric ids for found strings, marks the rest for creation.
3. **Deleted id recycling** — consume from `DELETED_IDS_KEY` first (see `getDeletedIds`); newly-recycled numeric ids are *fresh inserts to HNSW* even though the numeric id is reused, so `is_new_to_db` stays `true`. (This invariant is the Bug A fix; see "Recovery and known bugs" below.)
4. **New id generation** for any remainder via `get_next_ids` — atomic increment of `NEXT_ID_KEY`.
5. **WAL append** — every newly-assigned numeric id gets a `VECTOR_ADD` row in `op_log` through the same txn, so the id allocation and the recovery log entry commit atomically.
6. **Database write** — new `string → numeric_id` rows are written through the same txn. Bloom filter is updated after commit.

**Return Value:**
- Vector of `(numeric_id, is_new)` pairs
- `is_new = true`: New mapping created
- `is_new = false`: Existing mapping returned

### 2. ID Lookup - `get_id()`

```cpp
idInt get_id(MDBX_txn* txn, const std::string& str_id) const;
```

- Returns the numeric id or `0` if not found.
- Bloom filter is consulted first; only `might_exist` strings hit MDBX.

### 3. ID Deletion - `deletePoints()`

```cpp
std::vector<idInt> deletePoints(MDBX_txn* txn,
                                const std::vector<std::string>& external_ids);
```

- Looks up numeric ids for each string id.
- Deletes string→numeric rows from `id_map` through `txn`.
- Pushes the freed numeric ids onto `DELETED_IDS_KEY` via `add_to_deleted_ids` (inside the same txn).
- Returns the vector of freed numeric ids (`0` for not-found entries).

### 4. Deleted ID Retrieval - `getDeletedIds()`

```cpp
std::vector<idInt> getDeletedIds(MDBX_txn* txn, size_t max_count);
```

- Reads the `DELETED_IDS_KEY` array.
- Pops up to `max_count` ids from the head.
- Writes back the remainder (or deletes the key if empty) through `txn`.
- Returns the popped ids for reuse.
- The remainder is copied into a caller-owned `std::vector<idInt>` before being written back; the original `MDBX_val` from `mdbx_get` aliases the mapped page and using it as the source of `mdbx_put` under `MDBX_WRITEMAP` is documented UB.

## Atomicity and Crash Recovery

### Write-Ahead Log integration

- Every new numeric id allocated by `create_ids_batch` gets a `VECTOR_ADD` row appended to `op_log` through the same write txn (see `WriteAheadLog::append`).
- The append, the `id_map` rows, the vector bytes, the metadata, and the filter rows all commit as one MDBX transaction.
- See [docs/mdbx_shared_env_acid_revamp.md](mdbx_shared_env_acid_revamp.md) § HNSW Recovery for the full op_log lifecycle.

### Crash recovery (`IndexManager::recoverFromWAL`)

- WAL entries are replayed in sequence order.
- `VECTOR_ADD` whose vector row is absent → the add either never committed or was legitimately deleted later in the same save window. Failed ids are reclaimed via `reclaim_failed_ids` → `add_to_deleted_ids` (which dedups against existing entries; see "Recovery and known bugs").
- `VECTOR_DELETE` replays into HNSW only when the label exists and isn't already marked deleted.
- Replay is idempotent; HNSW is saved and `op_log` is cleared inside the same txn after a successful save.

### Recovery and known bugs (fixed on this branch)

Two production-grade bugs were found and fixed during crash-harness work. See [docs/single_txn_bug_investigation.md](single_txn_bug_investigation.md) for the full investigation; load-bearing invariants summarised here:

- **`create_ids_batch` no longer treats reused numeric ids as updates.** The earlier `is_reused` override forced HNSW down the `addPoint<false>` (rewire) path for any string id paired with a recycled numeric id, fracturing the graph. A fresh string id reusing a deleted numeric id is now `is_new_to_db = true` everywhere.
- **`add_to_deleted_ids` deduplicates before appending.** Recovery could otherwise re-push a numeric id that was *legitimately* deleted-then-killed mid-save-window, producing a duplicate in `DELETED_IDS_KEY` that handed the same id to two different string ids in the next batch.

## Bloom Filter Management

### Automatic Sizing Strategy

The bloom filter uses intelligent auto-sizing:

```cpp
size_t calculateOptimalBloomSize(size_t current_elements) const {
    return settings::calculateOptimalBloomSize(current_elements);
}
```

**Sizing Logic:**
- **Bit-based doubling**: Powers of 2 sizing for efficient memory usage
- **Growth triggers**: Rebuilds when capacity < optimal size
- **Custom sizing**: Supports user-specified minimum sizes
- **Performance optimization**: Balances memory usage vs. false positive rate

### Rebuild Process

Bloom filter rebuilding happens:
1. **On startup**: If file missing or undersized
2. **During operation**: When capacity insufficient
3. **After recovery**: To ensure consistency

**Rebuild Steps:**
1. Calculate optimal size based on current element count
2. Create new bloom filter with optimal capacity
3. Iterate through all MDBX keys (excluding special keys)
4. Add all string IDs to new bloom filter
5. Replace old filter and mark as modified
6. Save to disk

## Performance Characteristics

### Lookup Performance
- **Bloom Filter**: O(k) where k = number of hash functions (~3-4)
- **MDBX Lookup**: O(log n) B-tree lookup
- **Cache Locality**: MDBX uses memory mapping for efficiency

### Batch Operations
- **Amortized Cost**: Single transaction for multiple operations
- **Bloom Filter Batching**: Multiple additions before save
- **Reduced Syscalls**: Minimize transaction overhead

### Memory Usage
- **MDBX Map**: shares the per-index shared env's virtual address allocation; not a separate map.
- **Bloom Filter**: size based on element count (typically KB-MB range).
- **Working Set**: minimal resident memory due to memory mapping.

## Thread Safety

### Concurrency Model
- **MDBX transactions**: ACID; sticky-thread mode in effect (one read txn per OS thread; writes begin/use/end on one thread).
- **Read concurrency**: multiple concurrent readers supported.
- **Write serialization**: MDBX serialises writers at the env level. A single shared write txn covers ID-mapper writes plus every other per-index write for one logical request.

### Lock-Free Reads
- Read methods do not take internal locks; they read through the caller's `MDBX_TXN_RDONLY`.
- Bloom filter reads are atomic at the data-structure level.
- MDBX handles read isolation automatically.

## Error Handling

### MDBX Error Recovery
- **Map Full**: Automatic map size doubling and retry
- **Transaction Conflicts**: Automatic abort and cleanup
- **Corruption Detection**: MDBX integrity checks

### Bloom Filter Recovery
- **Load Failure**: Automatic rebuild from database
- **Size Mismatch**: Automatic resize and rebuild
- **File Corruption**: Fallback to database-only operation

### WAL Integration Errors
- **WAL Failure**: Operations continue (IDs may be lost but recovered)
- **Recovery Errors**: Failed IDs added to recycle pool
- **Consistency Checks**: Orphaned ID detection and cleanup

## Configuration

### Constructor parameters
```cpp
IDMapper(MDBX_env* env,                         // shared per-index env from SharedIndexEnv
         const std::string& index_id,           // for logs and on-disk bloom filter naming
         bool is_new = false,                   // new index? -> skip the disk-bloom-load path
         UserType user_type = UserType::Starter, // tier affects bloom sizing
         size_t custom_bloom_size = 0);         // optional override
```

### Sizing
- Storage is sized by the shared env (`settings::VECTOR_MAP_SIZE_BITS` / `_MAX_BITS`), not a per-component map.
- `BLOOM_FILTER_BITS` still drives bloom-filter capacity; override via `custom_bloom_size`.

## Usage patterns

### Vector addition (caller's write txn)
```cpp
MDBX_txn* txn = /* begun on shared env */;
auto mappings = id_mapper->create_ids_batch(txn, str_ids);
for (auto [numeric_id, is_new_to_db] : mappings) {
    if (is_new_to_db) {
        // Fresh insert: VECTOR_ADD already appended to op_log
    } else {
        // Upsert against existing live id
    }
}
// caller commits txn -> id_map + op_log row + vector bytes + meta + filter rows all land atomically
```

### Vector lookup
```cpp
MDBX_txn* read_txn = /* MDBX_TXN_RDONLY on shared env */;
idInt numeric_id = id_mapper->get_id(read_txn, "vec1");
// numeric_id == 0 means "not found"
```

### Vector Deletion
```cpp
// Delete vectors and recycle IDs
std::vector<std::string> to_delete = {"vec1", "vec2"};
auto deleted_ids = id_mapper->deletePoints(to_delete);

// deleted_ids can be reused for new vectors
```

## Best Practices

### Performance Optimization
1. **Batch Operations**: Use `create_ids_batch()` instead of individual calls
2. **Bloom Filter Maintenance**: Allow automatic rebuilding for optimal performance
3. **ID Recycling**: Enable deleted ID reuse to minimize ID space growth

### Reliability
1. **WAL Integration**: Always run `create_ids_batch` inside the caller's write txn so the `VECTOR_ADD` op_log append commits atomically with the data rows.
2. **Error Handling**: Check return values and handle exceptions.
3. **Regular Maintenance**: Monitor bloom filter hit rates.

### Monitoring
1. **ID Space Usage**: Monitor `get_count()` for growth patterns
2. **Bloom Filter Efficiency**: Track false positive rates
3. **Deleted ID Pool**: Monitor recycling effectiveness

## Implementation Notes

### Key Design Decisions

1. **MDBX Choice**: Provides ACID transactions, memory mapping, and excellent performance
2. **Bloom Filter Integration**: Reduces unnecessary MDBX lookups significantly
3. **ID Recycling**: Prevents ID space exhaustion in high-churn scenarios
4. **WAL Coordination**: Ensures no IDs are lost during crashes
5. **Batch Processing**: Amortizes transaction costs for better performance

### Future Considerations

1. **Sharding**: For very large ID spaces, consider database sharding
2. **Compression**: Large string IDs could benefit from compression
3. **Analytics**: Add metrics for monitoring and optimization
4. **Backup/Restore**: Integration with database backup systems

This documentation reflects the current implementation as of the analysis date and should be updated as the codebase evolves.