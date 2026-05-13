# Filters

Onboarding guide to the filter subsystem on the `filter_pass` branch. Read this
top-to-bottom. The "Caveats" sections call out behaviours that are
counter-intuitive or that the team has not yet fixed; treat them as load-bearing
context, not nitpicks.

The source files this doc maps to:

- [src/filter/filter.hpp](../src/filter/filter.hpp), [src/filter/filter.cpp](../src/filter/filter.cpp) — top-level `Filter` class, JSON parsing, schema, query dispatch.
- [src/filter/numeric_index.hpp](../src/filter/numeric_index.hpp), [src/filter/numeric_index.cpp](../src/filter/numeric_index.cpp) — `NumericIndex`, `Bucket`, sortable-key helpers.
- [src/filter/category_index.hpp](../src/filter/category_index.hpp), [src/filter/category_index.cpp](../src/filter/category_index.cpp) — `CategoryIndex`, the bitmap-per-key store used for strings and booleans.
- [src/storage/vector_storage.hpp](../src/storage/vector_storage.hpp) — wires the filter store to vectors and metadata (`store_vectors_batch`, `deleteFilter`, `updateFilter`).
- [src/core/ndd.hpp](../src/core/ndd.hpp) — `searchKNN`, `deleteVectorsByFilter`, `updateFilters`. The adaptive search path lives here.
- [src/core/types.hpp](../src/core/types.hpp) — `FilterParams`.
- [src/hnsw/hnswalg.h](../src/hnsw/hnswalg.h) — HNSW fatigue boost when a filter is active.
- [src/main.cpp](../src/main.cpp) — HTTP layer.

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
  |                                    +-- NumericIndex   numbers (Number = unified int/float)
  |                                    +-- CategoryIndex  strings + booleans
  |
MDBX                                   one filter env per index, multiple named DBIs
```

There is **one MDBX environment per index**, opened from
`<index_path>/filters`, with four named sub-databases:

| dbi              | what it holds                                              |
|------------------|------------------------------------------------------------|
| `<unnamed>`      | filter schema JSON under key `__ndd_schema_v1__`          |
| `numeric_forward`| `<field>:<id>` -> 4-byte sortable value (current value)    |
| `numeric_inverted`| bucket-key -> serialized `Bucket` (the inverted index)    |
| `category_idx`   | `<field>:<value>` -> serialized RoaringBitmap of ids       |

Geometry is bounded by `settings::FILTER_MAP_SIZE_BITS` / `_MAX_BITS`
(env-overridable via `NDD_FILTER_MAP_SIZE_BITS` / `_MAX_BITS`). Default min is
16 MiB, default max is 64 GiB.

---

## 2. Error code contract

Every public filter call returns `ndd::OperationResult<T>` (defined in
[src/utils/types.hpp](../src/utils/types.hpp)). The codes are a stable contract
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

Schema lives in the unnamed MDBX dbi under key `__ndd_schema_v1__` as a JSON
object `{ field_name -> FieldType }`. `FieldType` is:

```
Unknown = 0, String = 1, Number = 2, Bool = 4
```

Two important rules:

- **First-write-wins.** The first insert that mentions a field freezes its type.
  Later inserts that use a different type return code `3`. See
  `Filter::register_field_type` in [filter.cpp](../src/filter/filter.cpp).
- **JSON type drives `FieldType`.** `value.is_boolean()` → Bool,
  `value.is_number()` → Number, `value.is_string()` → String. There is no way
  to override.

Schema is loaded once on `Filter` construction and cached in
`schema_cache_` under `schema_mutex_`. Every register touches MDBX (one
read-write txn per new field), so first inserts after restart pay a per-field
cost.

### Caveats

- The schema persistence is **not** atomic with numeric/category writes. The
  schema commit happens inside `register_field_type` during validation
  ([filter.cpp:683](../src/filter/filter.cpp#L683)), before any data is
  written. A crash between schema commit and data write leaves a "registered
  but empty" field.
- Low-level `add_to_filter()` /  `add_to_filter_batch()` /
  `remove_from_filter()` write directly to the `CategoryIndex` and bypass
  schema registration entirely. They will happily create category entries for
  a field that the schema (or a later JSON insert) thinks is `Number`. The
  high-level `add_filters_from_json[_batch]` is the only schema-aware entry
  point. Treat the low-level methods as legacy.

---

## 4. Numbers: one float32 sortable domain

Every numeric value — both JSON integers and JSON floats — is funneled through
`Filter::sortable_from_json` ([filter.cpp:57](../src/filter/filter.cpp#L57))
which:

1. Rejects non-numeric or non-finite values (`code = 2`).
2. Calls `value.get<float>()` (float32, not double).
3. Normalizes signed zero.
4. Passes the float to `float_to_sortable` to get a `uint32_t` that sorts the
   same way as the original float.

`float_to_sortable` is the standard IEEE-754 trick: flip all bits if the sign
bit is set, otherwise flip just the sign bit
([numeric_index.cpp:21](../src/filter/numeric_index.cpp#L21)). It makes the
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
- **Strict comparisons (`$gt`, `$lt`)** use `std::nextafterf` on the float32
  bound. The "next representable" gap grows with magnitude, so the bound for
  `$gt 1e20` is very different from the bound for `$gt 1.0`. See
  `Filter::numeric_bound_from_comparison` in
  [filter.cpp:117](../src/filter/filter.cpp#L117).
- **Migration.** Older DBs that wrote integers through `int_to_sortable` will
  not interoperate with the float32 sortable keys. The numeric index has no
  version field; the only currently-supported migration is "rebuild the index."
  Both `numeric_index.cpp` and the inline comment in
  [filter.cpp:94](../src/filter/filter.cpp#L94) call this out.
- **Bucket density.** The float bit domain is less uniformly dense in the
  integer range than `int_to_sortable` was. Integer-heavy fields will create
  more buckets and walk more entries on wide range scans.
- **Large JSON integers.** `category_value_from_json` calls
  `value.get<int>()` for integer category values
  ([filter.cpp:91](../src/filter/filter.cpp#L91)). Values outside `int` are
  unsafe (nlohmann throws on overflow; we do not catch with a code-2 message
  yet).

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

Serialization (see `Bucket::serialize` /
`Bucket::deserialize` in [numeric_index.cpp:162](../src/filter/numeric_index.cpp#L162)):

```
[uint32_t bm_size][bitmap bytes][deltas (N * u16)][ids (N * u32)]
```

`N` is **derived** from the residual bytes after the bitmap:
`(iov_len - 4 - bm_size) / (sizeof(u16) + sizeof(idInt))`. The branch removed
the explicit count field — this is what the `e9cca02 numeric filters using
only floats` and the bitmap-only-bucket fix commits depend on, because it lets
`ids.size()` transiently exceed `MAX_SIZE` (slide-split fallthrough) without
overflowing a stored count.

### 5.2 Inserts

`NumericIndex::put_internal` ([numeric_index.cpp:720](../src/filter/numeric_index.cpp#L720)):

1. Look up the forward entry `<field>:<id>`. If present with the same value,
   no-op. If different, remove the id from its old bucket.
2. Upsert the forward entry to the new value.
3. Call `add_to_buckets` to add the id to the correct inverted bucket.

`add_to_buckets` ([numeric_index.cpp:448](../src/filter/numeric_index.cpp#L448))
walks back from `MDBX_SET_RANGE` to find the predecessor bucket whose
`[base, base+MAX_DELTA]` covers the value. If no such bucket exists, it
creates one keyed at the exact value. If the matching bucket is at
`MAX_SIZE`, it runs the **slide split**.

### 5.3 Slide split

A bucket whose `ids.size()` reaches `MAX_SIZE` (1024) is split at a
**value boundary**, not the median. We scan right (then left) from the median
to find the first index where `deltas[i] != deltas[i-1]`, then split there.
This guarantees the right bucket's key (`base + delta[split]`) differs from
the left bucket's key, so MDBX never sees duplicate keys.

If the bucket is **all duplicates of `base_value`** (no value boundary
anywhere), the split cannot succeed. We fall through and just append the new
entry, letting the bucket sit momentarily over `MAX_SIZE`:

- If the new value equals `base_value`, the duplicate run extends and the
  fallthrough repeats on the next insert.
- If the new value is greater than `base_value`, the bucket now has a value
  boundary; the very next insert into this bucket will slide-split cleanly.

This is the path that creates **bitmap-only ids** (see next section).

### 5.4 Saturated-duplicate path / bitmap-only ids

`Bucket::add` ([numeric_index.cpp:91](../src/filter/numeric_index.cpp#L91))
has this branch:

```cpp
if (delta_32 == 0 && ids.size() >= MAX_SIZE) {
    return;   // id only goes into summary_bitmap
}
```

When the bucket is saturated and the incoming value equals `base_value`, the
id is added to `summary_bitmap` only. The arrays don't grow. The bitmap is
the source of truth for membership.

Three places that depend on this:

1. **Range scan** ([numeric_index.cpp:1011](../src/filter/numeric_index.cpp#L1011))
   handles `bucket.ids.empty()` but `summary_bitmap` non-empty: include the
   bitmap iff `base_value` is in `[min_val, max_val]`.
2. **Partial-overlap scan**
   ([numeric_index.cpp:1049](../src/filter/numeric_index.cpp#L1049)) reconstructs
   the bitmap-only subset by `summary_bitmap` minus `{ ids[i] : deltas[i] != 0 }`.
3. **Slide split** ([numeric_index.cpp:629](../src/filter/numeric_index.cpp#L629))
   computes the left bucket's bitmap as `original_bitmap - right_bucket.ids`
   instead of rebuilding it from `ids[]`, which would lose bitmap-only entries.

### Caveats

- **`Bucket::is_empty()` looks at both `ids.empty()` and
  `summary_bitmap.isEmpty()`** ([numeric_index.cpp:306](../src/filter/numeric_index.cpp#L306)).
  This was a fix on this branch. Older versions only looked at `ids`, which
  would let a delete operation delete a bucket that still had bitmap-only ids.
- **Bucket size is not page-bounded.** `summary_bitmap` size depends on the
  user-space insertion pattern, not the entry count. A high-cardinality
  bucket can be much larger than an MDBX page. There is a TODO in the header
  to bound buckets by page size; today they are bounded only by
  `MAX_SIZE = 1024` on the array side.
- **Bitmap-only partial-overlap is expensive.** The reconstruction at
  [numeric_index.cpp:1069](../src/filter/numeric_index.cpp#L1069) copies the
  full bitmap then `remove()`s every delta-zero entry. For a bucket dominated
  by saturated duplicates this is a real cost.

### 5.5 Range scan: fast path

`NumericIndex::range` ([numeric_index.cpp:902](../src/filter/numeric_index.cpp#L902))
walks buckets forward from the start of the query. For every bucket whose
**entire `[base, base + MAX_DELTA]` extent** lies inside `[min_val, max_val]`,
it skips the full deserialize and reads only the `summary_bitmap`
(`Bucket::read_summary_bitmap`). This fires on every interior bucket of a wide
scan and is the reason wide ranges only pay deltas/ids parsing on the start
and end buckets.

**Caveat:** the fast path is conservative — it requires the **declared
extent** to be covered, not the actual `[bucket_min, bucket_max]`. A bucket
packed tightly inside its extent still pays the deserialize unless the whole
65 K-wide window is inside the query. The TODO is to store actual bucket
min/max in the bucket header.

### 5.6 Batch writes

`NumericIndex::put_batch` ([numeric_index.cpp:800](../src/filter/numeric_index.cpp#L800))
commits in **chunks of `BATCH_TXN_CHUNK_SIZE = 256`**. This caps each
write transaction's dirty-page footprint so MDBX cannot blow past the env
map size on a multi-thousand-entry batch (the `750e5d8` commit). The
trade-off is that the batch is not atomic across chunks.

---

## 6. Category / boolean index

`CategoryIndex` ([category_index.cpp](../src/filter/category_index.cpp))
maps a formatted key `<field>:<value>` to a `RoaringBitmap`. Booleans are
treated as a category with values `"0"` / `"1"`.

```
add(field, value, id):
    1) txn: read bitmap for key                  (read-only txn)
    2)      bitmap.add(id)
    3) txn: write bitmap                         (read-write txn)
```

Two transactions. `remove` is the same shape with `bitmap.remove(id)`.
`add_batch_by_key` uses `addMany` so the in-memory union is O(N) instead of
N individual `add()`s.

### Caveats

- **Read-modify-write across two txns is not atomic.** Two concurrent
  `add()` calls to the same key can produce a lost update (writer B's read
  predates writer A's commit). High-write workloads on a hot category need
  external serialization until this moves into a single txn.
- **The whole bitmap is rewritten on every `add`/`remove`.** For a hot
  category with millions of ids this is wasteful. Tracked in the perf TODO
  list (see [filter_todo.md](filter_todo.md)).
- **Empty keys are not garbage collected.** Removing the last id from a key
  leaves an empty bitmap in MDBX.
- **`$in` with an empty string is silently skipped.** `computeFilterBitmap`
  skips category values whose string form is empty
  ([filter.cpp:465](../src/filter/filter.cpp#L465)). An empty-string match
  cannot be expressed in the current shape.

---

## 7. Bitmap deserialization safety

Both indexes use a hardened deserialization helper:

- `Bucket::read_bitmap_payload` in
  [numeric_index.cpp:46](../src/filter/numeric_index.cpp#L46).
- `CategoryIndex::read_bitmap_payload` in
  [category_index.cpp:14](../src/filter/category_index.cpp#L14).

Both follow the same pattern:

1. `roaring_bitmap_portable_deserialize_size(bytes, len)` to verify the
   payload self-describes a complete bitmap with no trailing junk.
2. `RoaringBitmap::readSafe(bytes, len)` (the bounds-checked deserializer).
3. `roaring_bitmap_internal_validate` to catch malformed run/array
   containers.

Any failure returns `code = 200`. This is the `a46d0b8 safe filter bitmap
deserialization` commit. Before this landed, a corrupt or empty bucket
payload could be silently treated as an empty bitmap; now it surfaces as a
corruption error.

---

## 8. Query API and operators

Top-level entry points on `Filter`:

- `computeFilterBitmap(filter_array)` — returns the bitmap of ids matching
  the AND of all clauses.
- `getIdsMatchingFilter(filter_array)` — same, materialized as a vector.
- `countIdsMatchingFilter(filter_array)` — same, materialized as a size_t.
- `check_numeric(field, id, op, val)` — fast point check via the forward
  index; used by `VectorStorage::matches_filter`.

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

Operators supported (`computeFilterBitmap` in
[filter.cpp:372](../src/filter/filter.cpp#L372)):

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

- **All clauses materialize before intersecting.** There is no cardinality
  estimator and no "cheapest first" lazy evaluation despite what the older
  doc claimed. Every clause runs an MDBX read pass on its own
  read-only transaction; only **after** all of them complete does the AND
  start. See [filter.cpp:521-545](../src/filter/filter.cpp#L521).
- **No clause-level shared snapshot.** Each `$eq`/`$in`/`$range`/`$lt..`
  opens its own MDBX read txn. A concurrent writer can produce a result that
  mixes pre- and post-write snapshots across clauses. The operation lock in
  `searchKNN` is also intentionally disabled
  ([ndd.hpp:1633](../src/core/ndd.hpp#L1633)), so reads do not serialize
  against writes either.
- **Field name and `$in` value validation:** the field name must not contain
  `:` (it is the key delimiter). Same rule for category values.
  `validate_filter_key_component` ([filter.cpp:30](../src/filter/filter.cpp#L30))
  rejects on `:` and returns code `1`. Length, NUL bytes, control bytes, and
  MDBX max-key are **not** validated. Category values are capped at 255 chars
  ([filter.cpp:96](../src/filter/filter.cpp#L96)).
- **The old doc said category values may contain `:`.** They cannot. Code is
  authoritative; this version of the doc reflects the code.
- **Schema is only consulted via `schema_cache_` during search.** If a query
  arrives before any insert has touched the field, `type` defaults to
  `Unknown` and the query falls through to the category branch, which will
  just return an empty bitmap.

---

## 9. Vector storage integration

`VectorStorage` owns the `Filter` instance (`filter_store_`) and is the only
caller that needs to keep three stores (`vector_store_`, `meta_store_`,
`filter_store_`) in sync. The two pieces worth knowing:

### 9.1 Upsert cleanup (`store_vectors_batch`)

Implemented in
[vector_storage.hpp:781](../src/storage/vector_storage.hpp#L781). Four phases:

1. **Cleanup** — for every entry whose `is_new_to_db[i] == false` (i.e. the
   id-mapper says this id was already live), read its prior `meta.filter`
   and call `filter_store_->remove_filters_from_json(...)` to drop the old
   filter index entries. Without this, a "rename" upsert leaves the old
   filter still matchable.
2. **Vectors** — `vector_store_->store_vectors_batch`.
3. **Meta** — `meta_store_->store_meta_batch` (this is the moment
   `meta.filter` becomes the new value; cleanup HAD to happen before this).
4. **Filters** — `filter_store_->add_filters_from_json_batch`.

The `is_new_to_db` vector is the id-mapper's signal:

- `true` → fresh slot, or reuse of a deleted slot. Nothing to clean.
- `false` → existing live id, an upsert. Cleanup required.
- empty → legacy caller; cleanup is silently skipped to preserve old
  semantics. New callers always pass the signal.

### 9.2 `deleteFilter`

[vector_storage.hpp:1049](../src/storage/vector_storage.hpp#L1049). Removes
filter index entries AND clears `meta.filter` (only when it exactly matches
the input). This is the `b0e8425` commit — before it, `deleteFilter` only
touched the index, leaving `meta.filter` populated and drifted.

### Caveats

- **Cross-store atomicity is by design absent.** Vector, meta, filter, and
  schema writes each commit in their own MDBX txn. A crash between phases
  leaves torn state: e.g. the cleanup phase committed but phase 4 never ran,
  so the index entries are gone for a vector that still claims (via
  `meta.filter`) to have them. The operator-visible remedy is rebuild.
- **`store_vectors_batch` issues one extra MDBX read per upserted id** to
  fetch the prior `meta.filter`. Fresh inserts skip this. Heavy upsert
  workloads should expect that overhead.
- **The cleanup pass only protects new writes.** Drift accumulated before
  this branch landed will not be fixed automatically. A targeted rebuild is
  required to clean it up.
- **`meta.filter` is the source of truth for cleanup.** If `meta` is
  unreadable for a live id (torn earlier write), `store_vectors_batch`
  returns code `103` instead of silently overwriting — better to surface the
  inconsistency than to make it worse.

---

## 10. Search: filter-aware path

`IndexManager::searchKNN` in
[ndd.hpp:1614](../src/core/ndd.hpp#L1614). When `filter_array` is non-empty:

1. Compute the filter bitmap.
2. If sparse search is enabled, run the sparse query in another thread, with
   the filter bitmap passed in.
3. For the dense path, branch on the bitmap's cardinality (`card`):
   - `card == 0` → no dense results.
   - `card < params.prefilter_threshold` → **brute force on the small set**.
     Iterate the bitmap into `valid_ids`, visit those vectors via
     `visit_vectors_by_ids`, compute distances directly, keep a top-`k` heap.
     This bypasses HNSW.
   - Otherwise → **filtered HNSW**. Pass a `BitMapFilterFunctor` and
     `params.boost_percentage` to `HierarchicalNSW::searchKnn`.

`FilterParams` ([core/types.hpp](../src/core/types.hpp)):

```cpp
struct FilterParams {
    size_t prefilter_threshold = settings::PREFILTER_CARDINALITY_THRESHOLD;  // default 10_000
    size_t boost_percentage    = settings::FILTER_BOOST_PERCENTAGE;          // default 0
};
```

Both are accepted from the HTTP body under `filter_params` (see
[main.cpp:839](../src/main.cpp#L839)).

### HNSW fatigue boost

When `filter_boost_percentage > 0` and a filter is active,
`HierarchicalNSW::searchKnn` ([hnswalg.h:1490](../src/hnsw/hnswalg.h#L1490))
inflates the early-exit budget by `(100 + boost) / 100`. The intuition: with
a filter the graph is rejecting more candidates, so it pays to explore more
before giving up. Set `boost_percentage > 0` if recall drops on filtered
queries.

### Caveats

- **The operation lock is intentionally disabled in search**
  ([ndd.hpp:1632-1637](../src/core/ndd.hpp#L1632)). The comment is explicit:
  "We aren't using reader's lock here to enable reads while writing.
  TODO: check correctness when stressing the system." Filter results can be
  inconsistent under concurrent writes.
- **The brute-force branch reads vectors one-by-one via a visitor.** This is
  fast for sparse filters (small `card`) but degrades sharply if you raise
  `prefilter_threshold` past a few tens of thousands.
- **HNSW filter functors are called inside the inner search loop** — keep
  `bitmap.contains(id)` cheap. `BitMapFilterFunctor` wraps `RoaringBitmap`
  which is already fast, but custom functors should not allocate.
- **`matches_filter`** in
  [vector_storage.hpp:622](../src/storage/vector_storage.hpp#L622) is a
  separate point-check API for callers that already have a vector in hand
  (e.g. recovery). It tries the index for numeric clauses and parses
  `meta.filter` JSON for string/bool clauses. It is NOT used by the main
  search path.

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

Treat [docs/filter_todo.md](filter_todo.md) and
[docs/filter_issue_drafts.md](filter_issue_drafts.md) as the authoritative
TODO list. Highlights:

- **Atomicity.** Schema, numeric, category, and meta writes are independent
  txns. Crash recovery needs a journal or single-txn execution; meanwhile
  rebuild is the only safe recovery.
- **Concurrent category writes.** Read-modify-write across two txns can
  drop concurrent updates to the same key.
- **Search snapshot consistency.** Filter clauses each open their own read
  txn, and `searchKNN` skips the operation lock. Multi-clause results may
  mix snapshots.
- **Schema bypass by low-level category APIs.** `add_to_filter()` does not
  consult the schema.
- **Numeric bucket format is unversioned.** Old DBs need rebuild; tests
  explicitly reject the legacy count-prefixed payload.
- **Fast path is coarse.** Tight buckets inside their extent still
  deserialize.
- **`$in` issues one MDBX read txn per value.** Batch under one txn.
- **Cardinality estimator does not exist.** All partial bitmaps materialize
  before intersection.

If you are about to land filter changes, scan §3-9 caveats first and check
whether your change closes any of them.
