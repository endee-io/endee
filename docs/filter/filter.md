# Filters

- Onboarding guide to the filter subsystem.
- Read top-to-bottom; the "Caveats" subsections call out non-obvious behaviour treat them as correctness-critical.

Source files this doc maps to:

- [src/filter/filter.hpp](../../src/filter/filter.hpp), [src/filter/filter.cpp](../../src/filter/filter.cpp) — top-level `Filter` class, JSON parsing, schema, query dispatch.
- [src/filter/numeric_index.hpp](../../src/filter/numeric_index.hpp), [src/filter/numeric_index.cpp](../../src/filter/numeric_index.cpp) — `NumericIndex`, `Bucket`, sortable-key helpers.
- [src/filter/category_index.hpp](../../src/filter/category_index.hpp), [src/filter/category_index.cpp](../../src/filter/category_index.cpp) — `CategoryIndex`, the bitmap-per-key store used for strings and booleans.
- [src/storage/vector_storage.hpp](../../src/storage/vector_storage.hpp) — wires the filter store to vectors and metadata (`store_vectors_batch`, `deleteFilter`, `updateFilter`).
- [src/core/ndd.hpp](../../src/core/ndd.hpp) — `searchKNN`, `deleteVectorsByFilter`, `updateFilters`; adaptive search path lives here.
- [src/core/types.hpp](../../src/core/types.hpp) — `FilterParams`.
- [src/hnsw/hnswalg.h](../../src/hnsw/hnswalg.h) — HNSW fatigue boost when a filter is active.
- [src/main.cpp](../../src/main.cpp) — HTTP layer.

---

## 1. Big picture

```
HTTP                main.cpp           parses request, builds filter_array JSON
  |
IndexManager        ndd.hpp            searchKNN / insert / delete / update
  |
VectorStorage       vector_storage.hpp owns filter_store_ + meta_store_ + vector_store_
  |
Filter              filter/filter.cpp  schema + JSON-to-index dispatch
  |                                    |
  |                                    +-- NumericIndex   numbers (float32 sortable domain)
  |                                    +-- CategoryIndex  strings + booleans
  |
MDBX                                   shared per-index env; filter DBIs alongside vector/meta DBIs
```

- The filter subsystem does **not** own its own MDBX env. `Filter::Filter(MDBX_env*, ...)` takes the shared env created by `SharedIndexEnv` ([src/storage/shared_mdbx.hpp](../../src/storage/shared_mdbx.hpp)) and opens DBIs inside it. See [docs/mdbx_shared_env_acid_revamp.md](../mdbx_shared_env_acid_revamp.md) for the full layout.
- Filter-related DBIs in the shared env:

| dbi              | what it holds                                              |
|------------------|------------------------------------------------------------|
| `filter_schema`  | filter schema JSON under key `__ndd_schema_v1__`           |
| `numeric_forward`| `<field>:<id>` → 4-byte sortable value (current value)     |
| `numeric_inverted`| bucket-key → serialized `Bucket` (the inverted index)     |
| `category_idx`   | `<field>:<value>` → serialized RoaringBitmap of ids        |

- Env geometry is bounded by `settings::VECTOR_MAP_SIZE_BITS` / `_MAX_BITS` (the whole shared env). The legacy `NDD_FILTER_MAP_SIZE_*` env vars no longer size a separate filter env.

---

## 2. Error code contract

Every public filter call returns `ndd::OperationResult<T>` (defined in
[src/utils/types.hpp](../../src/utils/types.hpp)). The codes are a stable contract
between the filter layer and `main.cpp`:

| code      | meaning                                                          | HTTP |
|-----------|------------------------------------------------------------------|------|
| `0`       | success                                                          | 2xx  |
| `1`       | invalid JSON shape (not an array / not an object / bad keys)     | 400  |
| `2`       | unsupported operator or invalid value for the field type         | 400  |
| `3`       | field type conflict with the persisted schema                    | 400  |
| `100-199` | MDBX / storage failure                                           | 500  |
| `200-299` | corruption / invariant violation                                 | 500  |

`main.cpp::operation_error_is_client_error` returns true for `code < 100`.
Doc-comments on every public method spell out the per-call code range.

---

## 3. Filter schema and field types

Schema lives in the `filter_schema` MDBX DBI inside the shared env, under key `__ndd_schema_v1__`, as a JSON
object `{ field_name -> FieldType }`. `FieldType` is:

```
Unknown = 0, String = 1, Number = 2, Bool = 4
```

Two important rules:

- **First-write-wins.** The first insert that mentions a field freezes its type.
  Later inserts that use a different type return code `3`. See
  `Filter::register_field_type` in [filter.cpp](../../src/filter/filter.cpp).
- **JSON type drives `FieldType`.** `value.is_boolean()` → Bool,
  `value.is_number()` → Number, `value.is_string()` → String. There is no way
  to override.

Schema is loaded once on `Filter` construction and cached in
`schema_cache_` under `schema_mutex_`. Every register touches MDBX (one
read-write txn per new field), so first inserts after restart pay a per-field
cost.

### Caveats

- **Schema persistence is now atomic with data writes.** On the shared-env layout, transactional filter writes build a local schema snapshot and write it through the caller's MDBX txn alongside the numeric/category rows. The in-memory `schema_cache_` is reloaded only after commit, so an abort cannot publish phantom schema fields. See `docs/mdbx_shared_env_acid_revamp.md` § Transaction Model.
- **Low-level helpers bypass the schema.** `add_to_filter()` / `add_to_filter_batch()` / `remove_from_filter()` write directly to the `CategoryIndex` and bypass schema registration. They will happily create category entries for a field that the schema (or a later JSON insert) thinks is `Number`. The high-level `add_filters_from_json[_batch]` is the only schema-aware entry point. Treat the low-level methods as legacy.

---

## 4. Numbers: one float32 sortable domain

Every numeric value — both JSON integers and JSON floats — is funneled through
`Filter::sortable_from_json` ([filter.cpp](../../src/filter/filter.cpp))
which:

1. Rejects non-numeric or non-finite values (`code = 2`).
2. Calls `value.get<float>()` (float32, not double).
3. Normalizes signed zero.
4. Passes the float to `float_to_sortable` to get a `uint32_t` that sorts the
   same way as the original float.

`float_to_sortable` is the standard IEEE-754 trick: flip all bits if the sign
bit is set, otherwise flip just the sign bit
([numeric_index.cpp](../../src/filter/numeric_index.cpp)). It makes the
representation lexicographically ordered, which means we can scan inverted
buckets with a normal MDBX cursor and get range semantics.

There is also `int_to_sortable` in the same file. **It is no longer used by
inserts or queries.** All numeric paths go through `float_to_sortable`. The
function is left in the source for tests and for a potential future
"true integer" type.

### Caveats (read this before debugging an off-by-one)

- **float32 precision.** Above `2^24 = 16,777,216`, not every integer is
  representable in float32. `1 vs 1.0` compare equal (good) but
  `16_777_217 vs 16_777_216` collapse to the same key (bad). The doc comment
  above `sortable_from_json` spells this out.
- **Strict comparisons (`$gt`, `$lt`)** use `std::nextafterf` on the float32 bound. The "next representable" gap grows with magnitude, so the bound for `$gt 1e20` is very different from the bound for `$gt 1.0`. See `Filter::numeric_bound_from_comparison` in [filter.cpp](../../src/filter/filter.cpp).
- **Migration.** Older DBs that wrote integers through `int_to_sortable` will not interoperate with the float32 sortable keys. The numeric index has no version field; the only currently-supported migration is "rebuild the index." Both `numeric_index.cpp` and an inline comment in `filter.cpp` call this out.
- **Bucket density.** The float bit domain is less uniformly dense in the integer range than `int_to_sortable` was. Integer-heavy fields create more buckets and walk more entries on wide range scans.
- **Large JSON integers.** `category_value_from_json` calls `value.get<int>()` for integer category values. Values outside `int` are unsafe (nlohmann throws on overflow; we don't yet catch with a code-2 message).

---

## 5. Numeric inverted index

Owned by `NumericIndex`. The data model is a B+-tree of fixed-width buckets
keyed by `<field>:<base_value_big_endian>`.

### 5.1 Bucket layout

```cpp
struct Bucket {
    static constexpr size_t MAX_SIZE = 1024;          // soft cap on ids.size()
    static constexpr uint32_t MAX_DELTA = 65535;      // u16 max
    uint32_t base_value = 0;                          // runtime only

    std::vector<uint16_t>   deltas;                   // sorted ascending
    std::vector<ndd::idInt> ids;                      // index-aligned with deltas
    ndd::RoaringBitmap      summary_bitmap;           // union of all ids
};
```

Serialization (see `Bucket::serialize` / `Bucket::deserialize` in [numeric_index.cpp](../../src/filter/numeric_index.cpp)):

```
[uint32_t bm_size][bitmap bytes][deltas (N * u16)][ids (N * u32)]
```

- `N` is **derived** from the residual bytes after the bitmap: `(iov_len - 4 - bm_size) / (sizeof(u16) + sizeof(idInt))`.
- The branch removed the explicit count field — required so `ids.size()` can transiently exceed `MAX_SIZE` (slide-split fallthrough) without overflowing a stored count.

### 5.2 Inserts

`NumericIndex::put_internal`:

1. Look up the forward entry `<field>:<id>`. If present with the same value, no-op. If different, remove the id from its old bucket.
2. Upsert the forward entry to the new value.
3. Call `add_to_buckets` to add the id to the correct inverted bucket.

`add_to_buckets` walks back from `MDBX_SET_RANGE` to find the predecessor bucket whose `[base, base+MAX_DELTA]` covers the value. If no such bucket exists, creates one keyed at the exact value. If the matching bucket is at `MAX_SIZE`, runs the **slide split**.

### 5.3 Slide split

- Trigger: a bucket whose `ids.size()` reaches `MAX_SIZE` (1024).
- Splits at a **value boundary**, not the median: scan right (then left) from the median to find the first index where `deltas[i] != deltas[i-1]`, split there. This guarantees the right bucket's key (`base + delta[split]`) differs from the left bucket's, so MDBX never sees duplicate keys.
- If the bucket is **all duplicates of `base_value`** (no value boundary anywhere), the split cannot succeed. We fall through and append, letting the bucket sit momentarily over `MAX_SIZE`:
  - If the new value equals `base_value`, the duplicate run extends and the fallthrough repeats on the next insert.
  - If the new value is greater than `base_value`, the bucket now has a value boundary; the next insert slide-splits cleanly.
- This path creates **bitmap-only ids** (see next section).

### 5.4 Saturated-duplicate path / bitmap-only ids

`Bucket::add` has this branch:

```cpp
if (delta_32 == 0 && ids.size() >= MAX_SIZE) {
    return;   // id only goes into summary_bitmap
}
```

- When the bucket is saturated and the incoming value equals `base_value`, the id is added to `summary_bitmap` only. Arrays don't grow. Bitmap is the source of truth for membership.

Three places depend on this:

1. **Range scan** handles `bucket.ids.empty()` but `summary_bitmap` non-empty: include the bitmap iff `base_value ∈ [min_val, max_val]`.
2. **Partial-overlap scan** reconstructs the bitmap-only subset as `summary_bitmap - { ids[i] : deltas[i] != 0 }`.
3. **Slide split** computes the left bucket's bitmap as `original_bitmap - right_bucket.ids` instead of rebuilding from `ids[]`, which would lose bitmap-only entries.

### Caveats

- **`Bucket::is_empty()` checks both `ids.empty()` and `summary_bitmap.isEmpty()`.** Older versions only checked `ids`, which let a delete operation drop a bucket that still had bitmap-only ids.
- **Bucket size is not page-bounded.** `summary_bitmap` size depends on the user-space insertion pattern, not the entry count. A high-cardinality bucket can be much larger than an MDBX page. There is a TODO in the header to bound buckets by page size; today they are bounded only by `MAX_SIZE = 1024` on the array side.
- **Bitmap-only partial-overlap is expensive.** The reconstruction copies the full bitmap then `remove()`s every delta-zero entry. For a bucket dominated by saturated duplicates this is a real cost.

### 5.5 Range scan: fast path

- `NumericIndex::range` walks buckets forward from the start of the query.
- For every bucket whose **entire `[base, base + MAX_DELTA]` extent** lies inside `[min_val, max_val]`, it skips the full deserialize and reads only the `summary_bitmap` (`Bucket::read_summary_bitmap`).
- Fires on every interior bucket of a wide scan — wide ranges only pay deltas/ids parsing on the start and end buckets.
- **Caveat**: the fast path is conservative — it requires the **declared extent** to be covered, not the actual `[bucket_min, bucket_max]`. A bucket packed tightly inside its extent still pays the deserialize unless the whole 65 K-wide window is inside the query. TODO: store actual bucket min/max in the bucket header.

### 5.6 Batch writes

- The shared-txn path (`put_batch(MDBX_txn*, entries)`) processes every entry inside the caller's transaction — atomic with the rest of the surrounding add/upsert.
- The standalone legacy variant `put_batch(entries)` still self-chunks in `BATCH_TXN_CHUNK_SIZE = 256`-row batches to cap a write txn's dirty-page footprint, and is therefore not atomic across chunks. It is kept for tests and one-off utilities; loaded-index writes always go through the txn-taking variant.

---

## 6. Category / boolean index

- `CategoryIndex` ([category_index.cpp](../../src/filter/category_index.cpp)) maps a formatted key `<field>:<value>` to a `RoaringBitmap`.
- Booleans are treated as a category with values `"0"` / `"1"`.
- Public API (`add`, `remove`, `add_batch_by_key`, `get_bitmap`, `get_bitmap_by_key`) all take the caller's `MDBX_txn*` as the first parameter and never call `mdbx_txn_begin` themselves.

Shape of one `add` inside the caller's write txn:

```
add(txn, field, value, id):
    1) read bitmap for key from txn
    2) bitmap.add(id)
    3) write bitmap back through the same txn
```

- `remove` mirrors with `bitmap.remove(id)`.
- `add_batch_by_key` uses `addMany` so the in-memory union is O(N) instead of N individual `add()`s.

### Caveats

- **RMW is atomic now.** The read and write happen inside one MDBX write txn supplied by the caller, so two concurrent `add()` calls to the same key cannot produce a lost update — MDBX serialises writers at the env level.
- **The whole bitmap is rewritten on every `add`/`remove`.** For a hot category with millions of ids this is wasteful. Tracked as a perf follow-up; see [docs/followups.md](../followups.md) for the running list.
- **Empty keys are not garbage collected.** Removing the last id from a key leaves an empty bitmap in MDBX.
- **`$in` with an empty string is silently skipped.** `computeFilterBitmap` skips category values whose string form is empty. An empty-string match cannot be expressed in the current shape.

---

## 7. Bitmap deserialization safety

Both indexes use a hardened deserialization helper:

- `Bucket::read_bitmap_payload` in [numeric_index.cpp](../../src/filter/numeric_index.cpp).
- `CategoryIndex::read_bitmap_payload` in [category_index.cpp](../../src/filter/category_index.cpp).

Both follow the same pattern:

1. `roaring_bitmap_portable_deserialize_size(bytes, len)` to verify the payload self-describes a complete bitmap with no trailing junk.
2. `RoaringBitmap::readSafe(bytes, len)` (the bounds-checked deserializer).
3. `roaring_bitmap_internal_validate` to catch malformed run/array containers.

Any failure returns `code = 200`. Before this landed, a corrupt or empty bucket payload could be silently treated as an empty bitmap; now it surfaces as a corruption error.

---

## 8. Query API and operators

Top-level entry points on `Filter` — all take the caller's `MDBX_txn*` as the first argument so every clause reads from a single committed snapshot:

- `computeFilterBitmap(txn, filter_array)` — returns the bitmap of ids matching the AND of all clauses.
- `getIdsMatchingFilter(txn, filter_array)` — same, materialized as a vector.
- `countIdsMatchingFilter(txn, filter_array)` — same, materialized as a `size_t`.
- `check_numeric(txn, field, id, op, val)` — fast point check via the forward index; used by `VectorStorage::matches_filter`.

`filter_array` is a JSON **array** of single-field objects. Each clause uses
a Mongo-style `$op`:

```jsonc
[
    { "category":   { "$eq":    "books" } },
    { "in_stock":   { "$eq":    true    } },
    { "price":      { "$range": [10, 50] } },
    { "rating":     { "$gte":   4.0     } },
    { "discount":   { "$lt":    20      } },
    { "tags":       { "$in":    ["sale", "new"] } }
]
```

Operators supported (see `computeFilterBitmap` in [filter.cpp](../../src/filter/filter.cpp)):

| operator   | types         | notes                                                                 |
|------------|---------------|-----------------------------------------------------------------------|
| `$eq`      | any           | numeric → `range(v, v)`; category → bitmap lookup.                    |
| `$in`      | any           | array; numeric → per-item range; category → per-value bitmap union.   |
| `$range`   | Number        | `[start, end]` inclusive in float32-sortable order. Errors if start > end. |
| `$lt`      | Number        | uses `nextafterf(x, -inf)` to make the bound exclusive.               |
| `$lte`     | Number        | inclusive.                                                            |
| `$gt`      | Number        | uses `nextafterf(x, +inf)` to make the bound exclusive.               |
| `$gte`     | Number        | inclusive.                                                            |

After all clauses are evaluated, partial bitmaps are sorted by cardinality
ascending and AND-intersected smallest-first. The intersection short-circuits
as soon as the result is empty.

### Caveats

- **All clauses materialize before intersecting.** No cardinality estimator, no "cheapest first" lazy evaluation. Every clause runs a full MDBX read pass; only **after** all complete does the AND start.
- **One snapshot across clauses.** `computeFilterBitmap` takes the caller's `MDBX_txn*` (typically opened at the top of `searchKNN`) and threads it into every category lookup, numeric range, and `$in` per-value pass. All clauses observe the same committed state. The old "each clause opens its own read txn" behaviour is gone.
- **`searchKNN` still skips `operation_mutex`.** The reader lock is intentionally not taken so long writes don't starve reads (see the `XXX` comment near `searchKNN` in `src/core/ndd.hpp`). Reads see a self-consistent committed snapshot but are not serialised against writers.
- **Field name and `$in` value validation.** Field names must not contain `:` (the key delimiter). Same for category values. `validate_filter_key_component` rejects on `:` and returns code `1`. Length, NUL bytes, control bytes, and MDBX max-key are **not** validated. Category values are capped at 255 chars.
- **Schema is only consulted via `schema_cache_` during search.** If a query arrives before any insert has touched the field, `type` defaults to `Unknown` and the query falls through to the category branch, which returns an empty bitmap.

---

## 9. Vector storage integration

`VectorStorage` owns the `Filter` instance (`filter_store_`) and is the only
caller that needs to keep three stores (`vector_store_`, `meta_store_`,
`filter_store_`) in sync. The two pieces worth knowing:

### 9.1 Upsert cleanup (`store_vectors_batch`)

Implemented in [vector_storage.hpp](../../src/storage/vector_storage.hpp). Four phases inside the caller's shared write txn:

1. **Cleanup** — for every entry whose `is_new_to_db[i] == false` (id-mapper says this id was already live), read its prior `meta.filter` and call `filter_store_->remove_filters_from_json(...)` to drop the old filter index entries. Without this, a "rename" upsert leaves the old filter still matchable.
2. **Vectors** — `vector_store_->store_vectors_batch`.
3. **Meta** — `meta_store_->store_meta_batch` (this is the moment `meta.filter` becomes the new value; cleanup HAD to happen before this).
4. **Filters** — `filter_store_->add_filters_from_json_batch`.

The `is_new_to_db` vector is the id-mapper's signal:

- `true` → fresh slot, or reuse of a deleted slot. Nothing to clean.
- `false` → existing live id, an upsert. Cleanup required.
- empty → legacy caller; cleanup is silently skipped to preserve old semantics. New callers always pass the signal.

### 9.2 `deleteFilter`

- Removes filter index entries AND clears `meta.filter` (only when it exactly matches the input). Before this fix, `deleteFilter` only touched the index, leaving `meta.filter` populated and drifted.

### Caveats

- **Cross-store atomicity is now guaranteed.** Vector, meta, filter, and schema writes for a single add/upsert/delete all commit through the caller's shared MDBX txn. A crash before commit drops everything; a crash after commit publishes everything. The old "torn state across phases" failure mode is gone. See [docs/mdbx_shared_env_acid_revamp.md](../mdbx_shared_env_acid_revamp.md) § Transaction Model.
- **`store_vectors_batch` issues one extra MDBX read per upserted id** to fetch the prior `meta.filter`. Fresh inserts skip this. Heavy upsert workloads should expect that overhead.
- **The cleanup pass only protects new writes.** Drift accumulated before this branch landed will not be fixed automatically. A targeted rebuild is required to clean it up.
- **`meta.filter` is the source of truth for cleanup.** If `meta` is unreadable for a live id (torn earlier write), `store_vectors_batch` returns code `103` instead of silently overwriting — better to surface the inconsistency than make it worse.

---

## 10. Search: filter-aware path

`IndexManager::searchKNN` in [src/core/ndd.hpp](../../src/core/ndd.hpp). When `filter_array` is non-empty:

1. Open one `MDBX_TXN_RDONLY` on the shared env at the top of the function.
2. Compute the filter bitmap via `Filter::computeFilterBitmap(txn, filter_array)`.
3. If sparse search is enabled, the sparse query runs in `std::async` on a worker thread with its own read txn (sticky-thread mode forbids reusing the main txn) and the filter bitmap is passed in.
4. For the dense path, branch on the bitmap's cardinality (`card`):
   - `card == 0` → no dense results.
   - `card < params.prefilter_threshold` → **brute force on the small set**. Iterate the bitmap into `valid_ids`, visit those vectors via `visit_vectors_by_ids(txn, ...)`, compute distances directly, keep a top-`k` heap. Bypasses HNSW.
   - Otherwise → **filtered HNSW**. Pass a `BitMapFilterFunctor` and `params.boost_percentage` to `HierarchicalNSW::searchKnn`, threading `txn` into the `VectorFetcher` closure so every cache-miss read uses the request snapshot.

`FilterParams` ([core/types.hpp](../../src/core/types.hpp)):

```cpp
struct FilterParams {
    size_t prefilter_threshold = settings::PREFILTER_CARDINALITY_THRESHOLD;  // default 10_000
    size_t boost_percentage    = settings::FILTER_BOOST_PERCENTAGE;          // default 0
};
```

Both are accepted from the HTTP body under `filter_params` (see [main.cpp](../../src/main.cpp)).

### HNSW fatigue boost

- When `filter_boost_percentage > 0` and a filter is active, `HierarchicalNSW::searchKnn` inflates the early-exit budget by `(100 + boost) / 100`.
- Intuition: with a filter, the graph rejects more candidates, so it pays to explore more before giving up. Set `boost_percentage > 0` if recall drops on filtered queries.

### Caveats

- **`searchKNN` does not take `operation_mutex`.** The reader lock is intentionally skipped so long writes don't starve reads (see the `XXX` comment near `searchKNN` in `src/core/ndd.hpp`). One search sees one consistent committed snapshot but is not serialised against concurrent writers.
- **The brute-force branch reads vectors one-by-one via a visitor.** Fast for sparse filters (small `card`) but degrades sharply if you raise `prefilter_threshold` past a few tens of thousands.
- **HNSW filter functors are called inside the inner search loop** — keep `bitmap.contains(id)` cheap. `BitMapFilterFunctor` wraps `RoaringBitmap` which is already fast, but custom functors should not allocate.
- **`matches_filter`** in [vector_storage.hpp](../../src/storage/vector_storage.hpp) is a separate point-check API for callers that already have a vector in hand (e.g. recovery). It tries the index for numeric clauses and parses `meta.filter` JSON for string/bool clauses. It is NOT used by the main search path.

---

## 11. Limits and validation summary

What the public surface will reject (code `1` or `2`):

- `filter_array` not a JSON array.
- Any clause that is not a single-field object.
- Field name empty or containing `:`.
- `$op` value's JSON type mismatching the operator (`$range` not a 2-array,
  `$in` not an array, comparison value not a number).
- Non-finite numbers.
- Category values that are not string/integer/boolean, or that exceed 255
  bytes, or that contain `:`.
- `$range` with `start > end`.
- `$lt`/`$lte`/`$gt`/`$gte` on a non-Number field.

What it will **not** reject yet:

- Overly long field names.
- NUL or control bytes in field name or value.
- Keys that exceed MDBX max-key-size (manifests as code 100 from MDBX, not
  code 1).
- Two distinct large integers that collapse to the same float32 key (silent;
  see §4 caveats).

---

## 12. Open work / where the bodies are buried

Closed by the shared-env revamp (see [docs/mdbx_shared_env_acid_revamp.md](../mdbx_shared_env_acid_revamp.md)):

- ~~Schema/numeric/category/meta atomicity~~ — all share the caller's MDBX txn.
- ~~Concurrent category writes lose updates~~ — RMW happens inside the caller's single shared write txn.
- ~~Search snapshot inconsistency across clauses~~ — `computeFilterBitmap` threads one read txn through every clause.

Still open:

- **Schema bypass by low-level category APIs.** `add_to_filter()` / `add_to_filter_batch()` / `remove_from_filter()` do not consult the schema.
- **Numeric bucket format is unversioned.** Old DBs need rebuild; tests explicitly reject the legacy count-prefixed payload.
- **Fast path is coarse.** Tight buckets inside their declared extent still deserialize.
- **`$in` runs one cursor pass per value.** Could batch into one pass under the shared txn.
- **No cardinality estimator.** All partial bitmaps materialize before intersection.
- **`searchKNN` skips `operation_mutex`** — search and writes are not serialised; reads see a consistent committed snapshot but no protection against the writer that committed it.
- **Per-clause MDBX read passes still happen.** Sharing one txn fixes snapshot consistency but does not coalesce work — wide `$in` and many-clause filters still pay one MDBX walk per clause.

For non-filter follow-ups across the branch, see [docs/followups.md](../followups.md).

If you are about to land filter changes, scan §3-9 caveats first and check whether your change closes any of them.
