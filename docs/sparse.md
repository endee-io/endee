# Sparse Vector Search

## What this subsystem does

This is a sparse vector similarity search engine. Given a collection of documents — each represented as a sparse vector of `(term_id, weight)` pairs — it answers "which documents have the highest dot-product with this query vector?" efficiently.

It stores everything in MDBX (an embedded key-value database, similar to LMDB). There are two layers:

1. **Raw document store** — the source-of-truth sparse vectors, one row per document.
2. **Inverted index** — a derived structure that maps each term to the list of documents containing it, organized into fixed-size blocks for efficient streaming.

Both layers live in the same MDBX environment and are updated atomically within a single transaction.

## File map

| File | What it does |
|---|---|
| [sparse_vector.hpp](src/sparse/sparse_vector.hpp) | `SparseVector` struct: holds `(term_id, weight)` pairs, packs/unpacks them to a compact binary format |
| [sparse_storage.hpp](src/sparse/sparse_storage.hpp) | `SparseVectorStorage`: public API — open the DB, store/delete/search vectors, manage transactions |
| [inverted_index.hpp](src/sparse/inverted_index.hpp) | `InvertedIndex` class declaration, on-disk structs (`BlockHeader`, `PostingListHeader`), iterator |
| [inverted_index.cpp](src/sparse/inverted_index.cpp) | All the logic — search algorithm, block merge/save/load, quantization, SIMD helpers, pruning |

## Data types

`ndd::idInt` is `uint32_t` — this is the document ID type used throughout.

## SparseVector

A sparse vector is just two parallel arrays:

```cpp
struct SparseVector {
    std::vector<uint32_t> indices;  // term IDs, sorted ascending
    std::vector<float> values;      // weights, aligned with indices
};
```

### Packed binary format

When stored in MDBX, vectors are packed as:

```
[nr_nonzero : uint16_t] [term_ids : nr_nonzero × uint32_t] [values : nr_nonzero × fp16]
```

- `nr_nonzero` is `uint16_t`, so max 65535 terms per vector.
- Values are stored as IEEE FP16 (half-precision float) in the raw document table. Conversion is done inline (`float_to_fp16` / `fp16_to_float`).
- Constructor `SparseVector(const uint8_t*, size_t)` unpacks; `pack()` repacks.

## SparseVectorStorage — the public API

This is the class users interact with. It wraps the MDBX environment and exposes:

### Initialization

```cpp
SparseVectorStorage storage("/path/to/db");
storage.initialize();  // opens MDBX env, creates DBIs, loads term cache
```

MDBX is opened with flags `MDBX_NOSTICKYTHREADS | MDBX_NORDAHEAD | MDBX_LIFORECLAIM`, max size 1TB, max 10 named databases.

Two named databases (DBIs) are created:
- `sparse_docs` — raw vector store, keyed by `doc_id` (integer key)
- `blocked_term_postings` — inverted index blocks, keyed by packed `(term_id, block_nr)` (integer key)

### Storing vectors

```cpp
// Single insert via transaction
auto txn = storage.begin_transaction();
txn->store_vector(doc_id, sparse_vec);
txn->commit();

// Batch insert (preferred — fewer transactions)
storage.store_vectors_batch({{doc_id1, vec1}, {doc_id2, vec2}, ...});
```

Insert order: raw vector is written to `sparse_docs` first, then the inverted index is updated. Both happen in the same MDBX write transaction.

### Deleting vectors

```cpp
storage.delete_vector(doc_id);
// or via transaction:
txn->delete_vector(doc_id);
```

Delete order is reversed: read the raw vector, remove its terms from the inverted index, then delete the raw vector row.

### Searching

```cpp
auto results = storage.search(query_vec, k);
// returns vector<pair<doc_id, score>> sorted by score descending

// With a filter (only consider docs in the bitmap):
auto results = storage.search(query_vec, k, &roaring_filter);
```

### Concurrency

`SparseVectorStorage` has a `shared_mutex`:
- Writes (`store_vectors_batch`, `delete_vector`) take an exclusive lock.
- Search is delegated directly to `InvertedIndex`, which has its own `shared_mutex` (shared for search, exclusive for add/remove).

MDBX transactions and cursors are single-threaded — the search loop is not parallelized.

## Inverted index internals

### Key scheme

Every row in `blocked_term_postings` has a `uint64_t` key:

```
packed_key = (uint64(term_id) << 32) | uint64(block_nr)
```

This puts all rows for one term next to each other in MDBX's sorted key order, so you can seek to `(term_id, 0)` and iterate forward.

Reserved keys:
- `block_nr = UINT32_MAX` → this row is the **metadata row** for the term (stores `PostingListHeader`)
- `(term_id = UINT32_MAX, block_nr = 0)` → the **superblock** — a single row storing `SuperBlock` (format version metadata)
- `term_id = UINT32_MAX` is otherwise a reserved sentinel, rejected by all code paths

### Blocks

Documents are partitioned into fixed-size blocks:

```
block_nr     = doc_id / 65535
block_offset = doc_id % 65535    (uint16_t)
```

`kBlockCapacity = 65535` (`std::numeric_limits<uint16_t>::max()`). This means block offsets fit in 16 bits.

Each MDBX row for `(term_id, block_nr)` stores exactly the postings from that term that fall into that block's doc-id range. This is the fundamental design choice — writes are block-local merges, not whole-list rewrites.

### SuperBlock

A single metadata row stored at key `packPostingKey(UINT32_MAX, 0)`:

```cpp
struct SuperBlock {             // 1 byte, packed
    uint8_t format_version;     // must match settings::SPARSE_ONDISK_VERSION
};
```

On `initialize()`, the inverted index calls `validateSuperBlock()` which:
1. Reads the superblock row.
2. If not found and the DB is empty (fresh) → writes a new superblock with `format_version = settings::SPARSE_ONDISK_VERSION`.
3. If not found but the DB has existing rows → throws `std::runtime_error` (legacy incompatible database).
4. If found but `format_version != settings::SPARSE_ONDISK_VERSION` → throws `std::runtime_error` (version mismatch).

This key doesn't interfere with normal iteration: `loadTermInfo()` filters out `term_id == UINT32_MAX`, and `iterateTermBlocks()` seeks to specific term IDs that are never `UINT32_MAX`.

### Per-term metadata: PostingListHeader

Stored at key `(term_id, UINT32_MAX)`:

```cpp
struct PostingListHeader {       // 12 bytes, packed
    uint32_t nr_entries;         // total entries across all blocks (including tombstones)
    uint32_t nr_live_entries;    // entries with value > 0
    float    max_value;          // global max weight across all blocks
};
```

### Per-block payload

Stored at key `(term_id, block_nr)`:

```
[BlockHeader] [doc_offsets: n × uint16_t] [values: n × uint8_t (or float)]
```

```cpp
struct BlockHeader {             // 8 bytes, packed
    uint16_t nr_entries;
    uint16_t nr_live_entries;
    float    max_value;          // block-local max weight
};
```

- `doc_offsets[]` are sorted `uint16_t` values — the offset within the block (`doc_id % 65535`).
- `values[]` are the posting weights, either `uint8_t` (default, quantized) or `float` (when `NDD_INV_IDX_STORE_FLOATS` is defined).

### Quantization

By default, weights are quantized to `uint8_t` relative to the block's max value:

```
quantize(val, max_val) = round(val / max_val * 255)
                         clamped to [1, 255]    ← 0 means deleted (tombstone)

dequantize(val, max_val) = val * (max_val / 255)
```

The value `0` is reserved as a tombstone marker — it means the entry has been deleted but not yet compacted out of the block.

If `NDD_INV_IDX_STORE_FLOATS` is defined at compile time, values are stored as raw `float` and no quantization happens. The value `0.0f` (or ≤ 0) is still the tombstone.

### In-memory cache: term_info_

```cpp
std::unordered_map<uint32_t, float> term_info_;   // term_id → global max weight
```

Populated at startup by `loadTermInfo()`, which scans all metadata rows. Updated incrementally during add/remove. Used by search to:
1. Skip query terms that don't exist in the index.
2. Compute upper bounds for pruning (`upper_bound = global_max * query_weight`).

## Write path

### Batch insert: addDocumentsBatchInternal()

Given a batch of `(doc_id, SparseVector)` pairs:

1. **Pivot to term-major order.** Build a map: `term_id → [(doc_id, value), ...]`.

2. **For each term:**
   - Sort updates by `doc_id`, deduplicate (keep last value for duplicate doc_ids).
   - Split into sub-ranges by `block_nr`.
   - For each `(term_id, block_nr)` slice:
     - `loadBlockEntries()` — read and decode the existing block (if any) into a `vector<PostingListEntry>`.
     - Merge the existing entries and incoming updates as two sorted streams (classic merge-sort merge).
     - Recompute `new_live_count` and `new_block_max`.
     - `saveBlockEntries()` — serialize and write the merged block back to MDBX (or delete the block if empty).
   - Update the `PostingListHeader`:
     - Adjust `nr_entries` and `nr_live_entries` using deltas.
     - If the old global max might have been invalidated (the block that held it now has a lower max), call `recomputeGlobalMaxFromBlocks()` — a full scan of that term's block headers.
   - Update `term_info_`.

### Single delete: removeDocumentInternal()

For each term in the deleted vector:

1. Read the term's `PostingListHeader`.
2. Compute which `block_nr` the doc falls in.
3. `loadBlockEntries()` for that block.
4. Binary search for the `doc_id`.
5. Set its value to `0.0f` (tombstone).
6. If the tombstone ratio exceeds `INV_IDX_COMPACTION_TOMBSTONE_RATIO` (default 10%), compact the block in-place (remove all tombstones).
7. Recompute block stats, save or delete the block.
8. Update the `PostingListHeader` and `term_info_`.

## Search path

### Overview

Search computes the top-k documents by dot-product score with the query vector. It works by streaming through posting lists in doc-id order, accumulating scores in a dense buffer, and maintaining a min-heap of the best results.

### Phase 1: Build iterators

For each query term `(term_id, query_weight)`:
- Skip if `query_weight ≤ 0` or term not in `term_info_`.
- Read the term's `PostingListHeader`; skip if no live entries.
- Open an MDBX cursor.
- Create a `PostingListIterator` and call `init()`:
  - Seeks to the first block for this term.
  - Positions on the first live (non-tombstone) entry.
- If the iterator is not exhausted, keep it.

### Phase 2: Batch scoring loop

Search processes the doc-id space in windows of size `INV_IDX_SEARCH_BATCH_SZ` (default 10,000, configurable via `NDD_INV_IDX_SEARCH_BATCH_SZ` env var):

```
batch_start = min doc_id across all active iterators
batch_end   = batch_start + batch_size - 1
```

A dense `float` array `scores_buf[batch_size]` is zeroed. Then for each iterator:

**`accumulateBatchScores<StoreFloats>()`** — the hot inner loop:
- Walk through the current block's `doc_offsets[]` and `values[]`.
- For each live entry within `[batch_start, batch_end]`:
  - `local = block_base_doc_id - batch_start + offset`
  - `scores_buf[local] += dequantized_value * query_weight`
- When the current block is exhausted, `loadNextBlock()` and continue if still in range.
- Track `remaining_entries` for pruning.

### Phase 3: Extract top-k from batch

Scan `scores_buf`. For each non-zero score above the current threshold:
- Reconstruct `doc_id = batch_start + local`.
- If a RoaringBitmap filter exists, skip docs not in the filter.
- Push into a min-heap of size `k`. Update the threshold to the heap's minimum score.

### Phase 4: Compact and prune

- Remove exhausted iterators.
- If the heap is full and pruning is enabled (more than 1 iterator):
  - `pruneLongest()` finds the iterator with the most `remaining_entries`.
  - Computes `upper_bound = global_max * query_weight` for that iterator.
  - If `upper_bound ≤ current_threshold`, advance that iterator forward to the minimum doc_id among the *other* iterators (skipping a chunk of its posting list that can't contribute winners).
  - Only one list is pruned at a time.

### Finish

Close all cursors, abort the read-only transaction. Pop the heap into a vector and reverse it so results are in descending score order.

## PostingListIterator

A cursor-backed streaming iterator over one term's posting list. It never loads the entire list into memory — it reads one block at a time via zero-copy MDBX pointers.

Key methods:

| Method | What it does |
|---|---|
| `init()` | Seek to first block, position on first live entry |
| `loadFirstBlock()` | `MDBX_SET_RANGE` to `(term_id, 0)`, skip empty blocks |
| `loadNextBlock()` | `MDBX_NEXT`, stop when term_id changes or metadata row reached |
| `parseCurrentKV()` | Validate key, parse block payload into `BlockView`, set up zero-copy pointers |
| `advanceToNextLive()` | Skip tombstones in current block (uses SIMD for uint8 mode), load next block if needed |
| `next()` | Move to next live entry |
| `advance(target_doc_id)` | Block-aware seek — skip whole blocks if target is ahead, `lower_bound` within a block |
| `valueAt(idx)` | Dequantize and return the weight at position `idx` |
| `upperBound()` | `global_max * term_weight` — used for pruning decisions |

### BlockView

A zero-copy view into an MDBX value. Pointers are only valid while the cursor stays on the same record and the transaction is alive.

```cpp
struct BlockView {
    const uint16_t* doc_offsets;  // sorted block-local offsets
    const void*     values;       // uint8_t* or float* depending on mode
    uint32_t        count;
    uint8_t         value_bits;   // 8 or 32
    float           max_value;    // block-local max (needed for dequantization)
};
```

## SIMD helpers

Two SIMD-accelerated functions with implementations for AVX-512, AVX2, NEON, and SVE2 (plus scalar fallback):

- **`findNextLiveSIMD(values, size, start_idx)`** — finds the next non-zero byte in a `uint8_t` array. Used by `advanceToNextLive()` to skip tombstones quickly.
- **`findDocIdSIMD(doc_ids, size, start_idx, target)`** — finds the first `uint32_t` ≥ target. Currently not used by the main search path but available.

## Compile-time flags

| Flag | Effect |
|---|---|
| `NDD_INV_IDX_STORE_FLOATS` | Store block values as `float` instead of `uint8_t`. No quantization. |
| `ND_SPARSE_INSTRUMENT` | Enable timing instrumentation for search and update paths. Call `printSparseSearchDebugStats()` / `printSparseUpdateDebugStats()` to dump. |
| `NDD_INV_IDX_PRUNE_DEBUG` | Track how many entries each iterator skipped via pruning. Logged after search. |

## Runtime settings

| Setting | Default | Description |
|---|---|---|
| `INV_IDX_SEARCH_BATCH_SZ` | 10,000 | Size of the dense scoring window. Configurable via `NDD_INV_IDX_SEARCH_BATCH_SZ` env var. |
| `INV_IDX_COMPACTION_TOMBSTONE_RATIO` | 0.10 | When this fraction of a block's entries are tombstones during delete, compact in-place. |
| `NEAR_ZERO` | 1e-9 | Epsilon for float comparisons. |
| `SPARSE_ONDISK_VERSION` | 1 | Format version written to the superblock. Checked on load; mismatch throws. |

## Putting it all together — data flow diagram

```
                    User code
                       │
            ┌──────────▼──────────┐
            │ SparseVectorStorage │   ← public API, owns MDBX env
            │   shared_mutex      │
            └──┬──────────────┬───┘
               │              │
    ┌──────────▼──┐    ┌──────▼──────────┐
    │ sparse_docs │    │  InvertedIndex  │  ← owns blocked_term_postings DBI
    │  (MDBX DBI) │    │  shared_mutex   │
    │             │    │  term_info_     │  ← in-memory cache
    │ doc_id →    │    └──┬──────────┬───┘
    │  packed vec │       │          │
    └─────────────┘    write      search
                        │          │
              ┌─────────▼─┐  ┌─────▼──────────────┐
              │ term-major │  │ PostingListIterator │
              │ block-local│  │ (cursor-backed,     │
              │ merge      │  │  zero-copy blocks)  │
              └────────────┘  └─────────────────────┘
```

# Potential Performance Improvements

Several ideas could further improve performance:

1. Allow a configurable fraction of the lowest-weight query terms to be ignored at search time. This reduces the number of posting-list iterators, which can improve latency while preserving most of the recall. Users can tune the fraction on a per-query basis to balance speed and accuracy.
