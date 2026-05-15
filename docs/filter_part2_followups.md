# Filter `filter_safety` → `filter_pass` (Part 2) follow-ups

This document is the running record of everything from the `filter_pass` branch that **was not** brought into `filter_safety` (the Part-1 split). Each entry lists the original commit, what it does, why it was deferred, the on-disk or behavioral contract it changes, and the exact follow-up work needed when Part 2 is opened.

`filter_safety` ends at the commit `e56debe mac compile time flags to use xcrun to find the correct clang version`. Part 2 should branch from `filter_safety` (not from `master` or `filter_pass`) so that it inherits the validation, perf, and refactor work that Part 1 already paid for.

> **Why two parts at all?**
> Part 1 is byte-compatible with filter indexes built by `master`. A deployment can drop in `filter_safety` without rebuilding any data. Part 2 changes the on-disk bucket layout, the numeric sortable-key domain, and the upsert semantics — none of those can ship without a rebuild step, and bundling them with Part 1 would have forced every existing deployment to reindex just to pick up the `$gt`/`$lt` operators or the validation fixes.

---

## Index — Part 2 deferred items

1. [`546430d` — Numeric bucket on-disk layout (drops `count`, adds bitmap-only routing)](#1-546430d--numeric-bucket-on-disk-layout)
2. [`b0e8425` — Upsert cleanup pass and `deleteFilter` meta sync](#2-b0e8425--upsert-cleanup-and-deletefilter-meta-sync)
3. [`e9cca02` — Unified float32 numeric sortable domain](#3-e9cca02--unified-float32-numeric-sortable-domain)
4. [`4cb445d` — Query/removal/split handling of bitmap-only state](#4-4cb445d--bitmap-only-state-in-query--removal--split)
5. [`7743296` — `filter` headers split into `.cpp` + `.hpp`](#5-7743296--header--cpp-split-for-filter-)
6. [Part-2 portion of `02acc13` — vector_storage / numeric stress / repo_filter.py tests + Part-2 cases in `filter_test.cpp`](#6-02acc13-part-2-tests)

Cross-cutting items that Part 1 *temporarily* preserved to stay backward compatible and that Part 2 should clean up:

- [Part-1 carry: `count` field in `Bucket` serialization](#carry-1-bucket-count-field)
- [Part-1 carry: Read-side bitmap-only handling comment in `read_summary_bitmap`](#carry-2-read_summary_bitmap-comment)
- [Part-1 carry: `int_to_sortable` / `float_to_sortable` split in `sortable_from_json`](#carry-3-int_to_sortable--float_to_sortable-split)

---

## 1. `546430d` — Numeric bucket on-disk layout

**Why deferred**: changes the bytes on disk. A `master`-built bucket cannot be read by a branch carrying this commit; the new deserializer throws `"Bucket corrupt: residual bytes not aligned"`.

**What it changes**

- Bucket serialization format:
  - Old (Part 1): `[bm_size : u32][bitmap][count : u16][deltas : count*u16][ids : count*idInt]`
  - New (Part 2): `[bm_size : u32][bitmap][deltas][ids]` — `count` removed; recovered from `(iov_len - sizeof(u32) - bm_size) / (sizeof(u16) + sizeof(idInt))`.
- New saturated-duplicate routing in `Bucket::add` — when `delta_32 == 0` and `ids.size() >= MAX_SIZE`, the new id is added to `summary_bitmap` only (bitmap-only state) and the parallel arrays are not grown.
- `Bucket::remove` reads `summary_bitmap` as the source of truth instead of relying on a successful scan of `ids[]`.

**Affected files**: [src/filter/numeric_index.hpp](src/filter/numeric_index.hpp) (or its `.cpp` after item 5 lands).

**Part 2 checklist**
- [ ] Drop `count` field from `Bucket::serialize` / `Bucket::deserialize`. Make sure the residual-byte math in the new deserializer matches what `read_summary_bitmap` already expects.
- [ ] Implement the duplicate-cliff fix (`delta_32 == 0 && ids.size() >= MAX_SIZE` → bitmap-only insert).
- [ ] Implement the new `Bucket::remove` semantics (bitmap is source of truth; arrays cleaned best-effort).
- [ ] Provide a migration path: either bump a stored version sentinel and refuse to open old buckets, or perform an on-open conversion. Currently Part 1 silently rounds-trips old buckets through Part-1 serialize/deserialize.
- [ ] Pair this commit with item 4 (`4cb445d`) — query / range / split semantics depend on bitmap-only state existing.
- [ ] Remove the `Why count is intentionally ignored here` comment block in `read_summary_bitmap` (see [carry 2](#carry-2-read_summary_bitmap-comment)) — after this commit the comment is obsolete; replace it with a one-line note that residual bytes are pure data arrays.
- [ ] Remove the `GTEST_SKIP()` calls from `Hypothesis2.SaturationCreatesBitmapOnlyEntries`, `Hypothesis4.DeserializeRejectsLegacyCountFormat`, and `Hypothesis4.ReadSummaryBitmapRejectsLegacyCountFormat` in `tests/filter_test.cpp` — these are Part-2 regression alarms that Part 1 silenced because they assert behavior that doesn't exist yet. After this commit they must pass.

---

## 2. `b0e8425` — Upsert cleanup and `deleteFilter` meta sync

**Why deferred**: not a format change, but the patch itself states:
> "This patch only prevents NEW stale filter index entries from accumulating. It does not retroactively scrub entries left behind by previous upserts written before the fix landed. **A targeted rebuild is required to clean historical drift.**"

Any deployment that ran upserts on `master` carries stale filter index entries that no code path will clean up. To make the fix *correct* for those deployments, the operator must rebuild — same reindex requirement as a format change.

**What it changes**

- `store_vectors_batch(...)` gains a second parameter `const std::vector<bool>& is_new_to_db = {}`. When non-empty it must be the same size as `vectors`. The flag mirrors id_mapper's "was this str_id already mapped?" signal.
- New phase-1 cleanup pass: for each entry where `is_new_to_db[i] == false`, fetch the prior `meta.filter` via `meta_store_->get_meta(numeric_id).filter` and remove its category / numeric index entries before the new filter is written.
- `deleteFilter(...)` now clears `meta.filter` so subsequent `get_meta(...)` calls see the post-delete state.
- New error code path `103` ("Upsert cleanup: meta missing for numeric_id ...") for the torn-write case where id_mapper says the slot is live but meta cannot be loaded.

**Affected files**: [src/storage/vector_storage.hpp](src/storage/vector_storage.hpp), [src/core/ndd.hpp](src/core/ndd.hpp).

**Part 2 checklist**
- [ ] Add the `is_new_to_db` parameter and per-entry size check to `store_vectors_batch`.
- [ ] Implement the cleanup pass and the `103` error path.
- [ ] Update `ndd.hpp` call sites to thread the id_mapper signal (`numeric_ids[i].second`) into a parallel `is_new_to_db` vector.
- [ ] Implement `deleteFilter` meta sync.
- [ ] Document the rebuild requirement in the Part 2 release notes — operators upgrading from Part 1 with any history of upserts must reindex.
- [ ] Bring back `tests/vector_storage_test.cpp` from `filter_pass` (currently skipped — see item 6) and confirm it passes.

---

## 3. `e9cca02` — Unified float32 numeric sortable domain

**Why deferred**: encoding change. The commit message itself says
> "Existing filter DBs that indexed integers with `int_to_sortable` must be rebuilt."

A `master`-built field that indexed integers stores keys like `int_to_sortable(2) = 0x80000002`. The new code reads queries through `float_to_sortable(2.0f) = 0xC0000000`. These don't compare equal, so old indexes return wrong answers under the new query path.

**What it changes**

- `Filter::sortable_from_json` no longer special-cases `is_number_integer()`; every JSON numeric goes through `float_to_sortable(value.get<float>())`.
- `Filter::numeric_bound_from_comparison` ($gt / $gte / $lt / $lte) drops the integer-specific branches and uses `std::nextafterf` for strict bounds on all values.
- Rejects non-finite floats with HTTP 400 ("$op value must be a finite number").

**Affected files**: [src/filter/filter.cpp](src/filter/filter.cpp) and [src/filter/filter.hpp](src/filter/filter.hpp).

**Part 2 checklist**
- [ ] Replace the `is_number_integer()` branch in `sortable_from_json` with the unified float32 path.
- [ ] Strip the integer special-case from each of `$gt`, `$gte`, `$lt`, `$lte`.
- [ ] Add the finite-float32 check and the warn/reject path.
- [ ] Update the docstring on `sortable_from_json` and `numeric_bound_from_comparison` (the long version is in `e9cca02`'s diff — paste verbatim).
- [ ] Re-introduce the two Part-2 tests in `filter_test.cpp`: `IntegerIndexedNumericFieldCanBeQueriedWithFloatNumber` and `FloatIndexedNumericFieldCanBeQueriedWithIntegerNumber`, plus the `NumericRangeBench.FloatDomainVsIntegerDomain` benchmark.

---

## 4. `4cb445d` — Bitmap-only state in query / removal / split

**Why deferred**: mechanically a query-side change with no format change, but it is only load-bearing once item 1 (`546430d`) is in. Without bitmap-only state existing on disk, this commit is a no-op. Keeping it in Part 2 keeps the bucket logic coherent inside one PR.

**What it changes**

- `Bucket::is_empty()` from `return ids.empty();` to `return ids.empty() && summary_bitmap.isEmpty();`
- Slide-split bitmap rebuild stops doing `bitmap = empty; for(id : ids) bitmap.add(id)` — instead, subtracts only the ids that moved right (preserves delta-0 bitmap-only entries on the left).
- `range()` slow path: when `bucket.ids.empty()` but `summary_bitmap` is non-empty, include the bitmap iff `base_value ∈ [min_val, max_val]`.

**Affected files**: [src/filter/numeric_index.cpp](src/filter/numeric_index.cpp) (or [.hpp](src/filter/numeric_index.hpp) if item 5 hasn't landed yet).

**Part 2 checklist**
- [ ] Apply alongside item 1, not before. Verify the three test cases from `filter_test.cpp` (`SplitPreservesBitmapOnlyDuplicates`, `RemoveKeepsBucketAliveWithBitmapOnlyEntries`, `RangeSlowPathReturnsBitmapOnlyEntries`) pass after both land.

---

## 5. `7743296` — Header → cpp split for `filter/`

**Why deferred**: pure refactor (zero behavioral content), but the cpp file in `filter_pass` was authored *after* item 1 (`546430d`) landed in that branch, so it contains the Part-2 implementations of `Bucket::add`, `remove`, `serialize`, `deserialize`, `add_to_buckets`, and `range`. Cherry-picking it would either drag Part-2 semantics into Part 1, or force us to rewrite ~7 method bodies by hand. We chose to keep the implementations in `.hpp` for Part 1 and let Part 2 carry the split.

**What it changes**

- New files: `src/filter/category_index.cpp`, `src/filter/filter.cpp`, `src/filter/numeric_index.cpp`.
- `CMakeLists.txt`: new `ndd_filter` library target (`add_library(ndd_filter STATIC ${NDD_FILTER_SOURCES})`) with its own include directories and `MDB_MAXKEYSIZE=512` definition.
- All the corresponding `.hpp` files reduced to declarations only.

**Affected files**: `CMakeLists.txt`, `src/filter/*.hpp`, new `src/filter/*.cpp`.

**Part 2 checklist**
- [ ] Land the split *together with* items 1 and 4 (`546430d` + `4cb445d`) so the new `.cpp` files contain the Part-2 implementations from the start. This is the cleanest reorg: one commit replaces the inlined Part-1 bodies with declarations and drops the Part-2 bodies into the new `.cpp` files.
- [ ] Re-introduce the `add_library(ndd_filter STATIC ...)` target and its compile options. `-falign-functions=64` is already on all three Part-1 targets (`ndd_core`, `ndd_filter`-elect, `${NDD_BINARY_NAME}`) per the user's CMake decision — no flag work needed, just wire the new target.
- [ ] When the bucket implementation moves into the new `.cpp`, the comment block on `read_summary_bitmap` ([carry 2](#carry-2-read_summary_bitmap-comment)) should be condensed — the count field will be gone by then.

---

## 6. `02acc13` Part-2 tests

**Why deferred**: half of `02acc13`'s test content depends on Part-2 behavior and would not compile or pass against Part 1. We split the commit at cherry-pick time and committed only the Part-1 portion as `4bab3b9 testing (Part 1 subset)`.

**Already in Part 1** (`4bab3b9`):
- [tests/request_validation_test.cpp](tests/request_validation_test.cpp) — tests for `3e33557` filter parameter validation.
- The new `add_executable(ndd_request_validation_test ...)` block in [tests/CMakeLists.txt](tests/CMakeLists.txt) and the corresponding `gtest_discover_tests`.

**Deferred to Part 2**:
- `tests/vector_storage_test.cpp` — exercises `store_vectors_batch(..., is_new_to_db)` upsert cleanup and `deleteFilter` meta sync. Direct dependency on item 2.
- `tests/numeric_index_stress_test.cpp` — random churn + drain phase, asserts the forward↔inverted invariant across the bitmap-only / split paths. Direct dependency on items 1 and 4.
- `tests/repo_filter.py` — reproducer for the "65,536 duplicate cliff" fixed by item 1.
- Six `TEST_F` additions to `tests/filter_test.cpp`:
  - `IntegerIndexedNumericFieldCanBeQueriedWithFloatNumber` (item 3)
  - `FloatIndexedNumericFieldCanBeQueriedWithIntegerNumber` (item 3)
  - `SplitPreservesBitmapOnlyDuplicates` (items 1 + 4)
  - `RemoveKeepsBucketAliveWithBitmapOnlyEntries` (item 4)
  - `RangeSlowPathReturnsBitmapOnlyEntries` (item 4)
  - `NumericRangeBench.FloatDomainVsIntegerDomain` (item 3, benchmark)
- The `add_executable(ndd_vector_storage_test ...)` and `add_executable(ndd_numeric_index_stress_test ...)` blocks in [tests/CMakeLists.txt](tests/CMakeLists.txt) and their `gtest_discover_tests` calls.

**Part 2 checklist**
- [ ] When items 1–4 are all in, cherry-pick the *original* `02acc13` on top to recover the deferred files. The conflicts with `4bab3b9` will be small (the includes block at the top of `filter_test.cpp` is already taken; `tests/CMakeLists.txt` already has the request_validation_test block — just re-add the other two executables).

---

## Cross-cutting "Part 1 paid for backward compatibility" items

These were not deferred — they were *added* in Part 1 specifically so Part 1 could ship without a rebuild. Part 2 will simplify or remove them.

### Carry 1: Bucket `count` field

`Bucket::serialize` still writes a `uint16_t count` after the bitmap and before the deltas / ids arrays, and `Bucket::deserialize` still reads it. Master and pre-Part-1 deployments rely on this field, so Part 1 keeps it. Part 2 item 1 removes it.

**Why we kept it**: an existing on-disk bucket carries the `count` field. If Part 1 dropped it from the serializer, every round-trip (read + modify + write) would corrupt the bucket. The byte-length-derived count (item 1's approach) only works for buckets written by the new serializer.

### Carry 2: `read_summary_bitmap` comment

[src/filter/numeric_index.hpp](src/filter/numeric_index.hpp) lines ~219–263 carries a multi-paragraph comment on `read_summary_bitmap` that explains why the `count` field is intentionally not read by this function, and explicitly forecasts its removal. When item 1 lands, that comment becomes obsolete and should be condensed to a one-liner.

### Carry 3: `int_to_sortable` / `float_to_sortable` split

[src/filter/filter.hpp](src/filter/filter.hpp) `Filter::sortable_from_json` (and the four comparison operators in `numeric_bound_from_comparison`) still branch on `is_number_integer()`. This preserves the dual-domain encoding that master uses. Part 2 item 3 unifies these onto `float_to_sortable`.

---

## Verification snapshot (taken at end of Part 1)

These invariants must continue to hold at the tip of `filter_safety` until Part 2 begins:

- `Bucket::is_empty()` returns `ids.empty()` only — no bitmap check.
- `Bucket::serialize` writes `count` and `Bucket::deserialize` reads it (file: [src/filter/numeric_index.hpp](src/filter/numeric_index.hpp)).
- `Filter::sortable_from_json` has the `is_number_integer()` → `int_to_sortable` branch.
- `store_vectors_batch` is single-argument (no `is_new_to_db`).
- `Bucket::add` has no saturated-duplicate / bitmap-only path; every insert goes into the parallel arrays.
- `add_to_buckets` slide-split rebuilds the left bitmap from `ids[]` (the Part-1 way), not by subtracting moved ids.
- `tests/vector_storage_test.cpp` and `tests/numeric_index_stress_test.cpp` do not exist in the working tree.

If any of these flip during a future Part-1 maintenance commit, that commit has accidentally pulled in Part-2 semantics and should be reverted.
