# Filter `filter_safety_clean` → `filter_pass` (Part 2) follow-ups

This document is the running record of everything from the `filter_pass` branch that **was not** brought into `filter_safety_clean` (the Part-1 split — formerly `filter_safety`, renamed during cleanup). Each entry lists the original commit, what it does, why it was deferred, the on-disk or behavioral contract it changes, and the exact follow-up work needed when Part 2 is opened.

`filter_safety_clean` currently ends at the commit `9ccc2b3 filter: split headers into hpp + cpp` (with an unrelated `src/quant/common.hpp` touch-up at `abc2b1b` on top). Part 2 should branch from `filter_safety_clean` (not from `master` or `filter_pass`) so that it inherits the validation, perf, and refactor work that Part 1 already paid for — including [item 5 below](#5-7743296--header--cpp-split-for-filter--done-in-part-1), which has already been folded into Part 1 ahead of schedule.

> **Why two parts at all?**
> Part 1 is byte-compatible with filter indexes built by `master`. A deployment can drop in `filter_safety_clean` without rebuilding any data. Part 2 changes the on-disk bucket layout, the numeric sortable-key domain, and the upsert semantics — none of those can ship without a rebuild step, and bundling them with Part 1 would have forced every existing deployment to reindex just to pick up the `$gt`/`$lt` operators or the validation fixes.

---

## Index — Part 2 deferred items

1. [`546430d` — Numeric bucket on-disk layout (drops `count`, adds bitmap-only routing)](#1-546430d--numeric-bucket-on-disk-layout)
2. [`b0e8425` — Upsert cleanup pass and `deleteFilter` meta sync](#2-b0e8425--upsert-cleanup-and-deletefilter-meta-sync)
3. [`e9cca02` — Unified float32 numeric sortable domain](#3-e9cca02--unified-float32-numeric-sortable-domain)
4. [`4cb445d` — Query/removal/split handling of bitmap-only state](#4-4cb445d--bitmap-only-state-in-query--removal--split)
5. [`7743296` — `filter` headers split into `.cpp` + `.hpp`](#5-7743296--header--cpp-split-for-filter--done-in-part-1) — **done early in Part 1** (commit `9ccc2b3`)
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

**Affected files**: [src/filter/numeric_index.cpp](src/filter/numeric_index.cpp) — declarations stay in [src/filter/numeric_index.hpp](src/filter/numeric_index.hpp).

**Part 2 checklist**
- [ ] Drop `count` field from `Bucket::serialize` / `Bucket::deserialize`. Make sure the residual-byte math in the new deserializer matches what `read_summary_bitmap` already expects.
- [ ] Implement the duplicate-cliff fix (`delta_32 == 0 && ids.size() >= MAX_SIZE` → bitmap-only insert).
- [ ] Implement the new `Bucket::remove` semantics (bitmap is source of truth; arrays cleaned best-effort).
- [ ] Provide a migration path: either bump a stored version sentinel and refuse to open old buckets, or perform an on-open conversion. Currently Part 1 silently rounds-trips old buckets through Part-1 serialize/deserialize.
- [ ] Pair this commit with item 4 (`4cb445d`) — query / range / split semantics depend on bitmap-only state existing.
- [ ] Remove the `Why count is intentionally ignored here` comment block in `read_summary_bitmap` (see [carry 2](#carry-2-read_summary_bitmap-comment)) — after this commit the comment is obsolete; replace it with a one-line note that residual bytes are pure data arrays.
- [ ] Remove the `GTEST_SKIP()` calls from `Hypothesis2.SaturationCreatesBitmapOnlyEntries`, `Hypothesis4.DeserializeRejectsLegacyCountFormat`, and `Hypothesis4.ReadSummaryBitmapRejectsLegacyCountFormat` in [tests/filter_test.cpp](tests/filter_test.cpp) — these are Part-2 regression alarms that Part 1 silenced because they assert behavior that doesn't exist yet. After this commit they must pass. (The skip messages point at the doc's pre-rename name `docs/filter_part2_followups.md`; deleting the skips also clears that stale pointer — no separate fix needed.)

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

**Affected files**: [src/filter/numeric_index.cpp](src/filter/numeric_index.cpp).

**Part 2 checklist**
- [ ] Apply alongside item 1, not before. Verify the three test cases from `filter_test.cpp` (`SplitPreservesBitmapOnlyDuplicates`, `RemoveKeepsBucketAliveWithBitmapOnlyEntries`, `RangeSlowPathReturnsBitmapOnlyEntries`) pass after both land.

---

## 5. `7743296` — Header → cpp split for `filter/` — DONE in Part 1

**Status**: landed standalone in commit `9ccc2b3 filter: split headers into hpp + cpp`. It was *not* bundled with items 1 + 4 as the original checklist had recommended — the refactor was kept isolated so the diff stayed reviewable, and the Part-2 method bodies will land directly into the existing `.cpp` files when items 1 and 4 arrive.

**What landed**

- New translation units: [src/filter/category_index.cpp](src/filter/category_index.cpp), [src/filter/filter.cpp](src/filter/filter.cpp), [src/filter/numeric_index.cpp](src/filter/numeric_index.cpp). The corresponding `.hpp` files now hold only declarations plus a small set of inline accessors (`Bucket::is_empty`, `Bucket::get_value`, `Bucket::is_full`, the `sortable_from_float` family).
- Implementations were transcribed from the Part-1 `.hpp` bodies, *not* taken from `filter_pass`'s cpp — so they do not contain Part-2 semantics. Every Part-1 behavioral invariant in the [verification snapshot](#verification-snapshot-taken-at-end-of-part-1) was re-checked against the new cpp files and still holds.
- `#include <thread>` was added to [src/utils/settings.hpp](src/utils/settings.hpp) — it was relying on a transitive include from the old fat `filter.hpp`, and the trimmed header no longer pulls in `<thread>`.

**CMake wiring — deviation from the original plan**

The original checklist anticipated a separate `add_library(ndd_filter STATIC ${NDD_FILTER_SOURCES})` target. The implementation instead defines `NDD_FILTER_SOURCES` once in the root [CMakeLists.txt](CMakeLists.txt) and folds it into `NDD_CORE_SOURCES`; [tests/CMakeLists.txt](tests/CMakeLists.txt) pulls the same source set into the `ndd_filter_test` target. Both `ndd-avx2` and the filter test binary therefore link the same translation units without a separate static library hop. **Do not "fix" this back to a separate library** — there is no compile-flag divergence between filter sources and the rest of `NDD_CORE_SOURCES` that would justify one.

**Residual Part-2 work that originally lived here**

The "condense the `read_summary_bitmap` comment when the bucket impl moves to .cpp" task no longer makes sense as part of item 5 — the bucket impl is already in .cpp, but the comment block ([src/filter/numeric_index.hpp:104-140](src/filter/numeric_index.hpp#L104-L140)) is intentionally still there because the `count` field is still part of the layout. The comment becomes obsolete when item 1 (`546430d`) removes the field. That bullet now lives in item 1's checklist and in [carry 2](#carry-2-read_summary_bitmap-comment).

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

These invariants must continue to hold at the tip of `filter_safety_clean` until Part 2 begins. File paths reflect the post-split layout (commit `9ccc2b3`):

- `Bucket::is_empty()` returns `ids.empty()` only — no bitmap check ([src/filter/numeric_index.hpp:144](src/filter/numeric_index.hpp#L144), still an inline accessor in the header).
- `Bucket::serialize` writes `count` and `Bucket::deserialize` reads it ([src/filter/numeric_index.cpp:96-188](src/filter/numeric_index.cpp#L96-L188)).
- `Filter::sortable_from_json` has the `is_number_integer()` → `int_to_sortable` branch ([src/filter/filter.cpp:103-112](src/filter/filter.cpp#L103-L112)); the four bound operators retain their integer special-cases ([src/filter/filter.cpp:150-210](src/filter/filter.cpp#L150-L210)).
- `store_vectors_batch` is single-argument, no `is_new_to_db` ([src/storage/vector_storage.hpp:741](src/storage/vector_storage.hpp#L741)).
- `Bucket::add` has no saturated-duplicate / bitmap-only path; every insert goes into the parallel arrays ([src/filter/numeric_index.cpp:56-78](src/filter/numeric_index.cpp#L56-L78)).
- `Bucket::remove` scans `ids[]` linearly rather than treating `summary_bitmap` as source of truth ([src/filter/numeric_index.cpp:80-94](src/filter/numeric_index.cpp#L80-L94)).
- `add_to_buckets` slide-split rebuilds the left bitmap from `ids[]` (the Part-1 way), not by subtracting moved ids ([src/filter/numeric_index.cpp:499-503](src/filter/numeric_index.cpp#L499-L503)).
- `Bucket::range()` has no bitmap-only fallback in the slow path ([src/filter/numeric_index.cpp:828-847](src/filter/numeric_index.cpp#L828-L847)).
- [tests/vector_storage_test.cpp](tests/vector_storage_test.cpp), [tests/numeric_index_stress_test.cpp](tests/numeric_index_stress_test.cpp), and [tests/repo_filter.py](tests/repo_filter.py) do not exist in the working tree.

If any of these flip during a future Part-1 maintenance commit, that commit has accidentally pulled in Part-2 semantics and should be reverted.

---

## Part 2 PR — execution playbook

Operational notes for the engineer opening the Part-2 PR. The "what" is in each item's section above; this section is the "how".

### Recommended commit order inside the PR

Land the deferred commits in this order so each commit leaves the test suite green:

| # | SHA (on `filter_pass`) | Item | Coupling | Why this slot |
| --- | --- | --- | --- | --- |
| 1 | `546430d` | Item 1 — bucket layout drop `count` + bitmap-only routing | Pair with #2 | Format change; arrives first so the layout is settled before the query-side patch lands on top |
| 2 | `4cb445d` | Item 4 — query/removal/split handle bitmap-only state | Pair with #1 | Without #1 this is a no-op; without #2 the new layout's three bitmap-only tests in `filter_test.cpp` fail. **Squash #1 + #2 into a single commit** if reviewer prefers; do not merge them individually. |
| 3 | `e9cca02` | Item 3 — unified float32 sortable domain | Independent | Encoding change. Touches `filter.cpp` only; safe to land any time after #1+#2. |
| 4 | `b0e8425` | Item 2 — upsert cleanup + `deleteFilter` meta sync | Independent | Touches `vector_storage.hpp` + `ndd.hpp`; orthogonal to the bucket layout but shares the "operators must reindex" release-note bullet. |
| 5 | `02acc13` (partial) | Item 6 — vector_storage / stress / repo_filter tests + Part-2 cases in filter_test | Lands last | Depends on every code change above. Cherry-pick the original commit and resolve the small conflicts with `4bab3b9` (see item 6). |

Item 5 (`7743296` — header/cpp split) is already on `filter_safety_clean` as `9ccc2b3` and must **not** be cherry-picked again.

### Cherry-pick recipe

`filter_pass` is on the `upstream` and `source-repo` remotes but is not a local branch — that's fine. Cherry-pick by SHA; the objects are already in the local DB:

```bash
git fetch upstream filter_pass    # only needed if SHAs aren't already local
git checkout -b filter_part2 filter_safety_clean

# Pair item 1 + item 4 into one commit (recommended).
git cherry-pick -n 546430d 4cb445d
git commit -m "filter: drop bucket count field; add bitmap-only routing (items 1+4)"

# Items 3 and 2, each as its own commit:
git cherry-pick e9cca02
git cherry-pick b0e8425

# Item 6 last; expect conflicts in filter_test.cpp (includes block) and tests/CMakeLists.txt
# (request_validation_test block already present). Resolve by KEEPING the Part-1 version
# of the conflicting hunks and re-adding only the deferred test files / executables.
git cherry-pick 02acc13
```

Inside the squashed item 1+4 commit, also delete the three `GTEST_SKIP()` calls in [tests/filter_test.cpp](tests/filter_test.cpp) (Hypothesis2.SaturationCreatesBitmapOnlyEntries, Hypothesis4.DeserializeRejectsLegacyCountFormat, Hypothesis4.ReadSummaryBitmapRejectsLegacyCountFormat) and condense the multi-paragraph `read_summary_bitmap` comment at [src/filter/numeric_index.hpp:104-140](src/filter/numeric_index.hpp#L104-L140) into a one-liner — both are tracked in item 1's checklist.

### Open design decisions to settle *before* writing code

The doc has flagged these but never picked an answer. Resolve them in a thread on the PR description before opening for review.

1. **Migration story for existing on-disk buckets.** Two options:
   - **Version sentinel + refuse-to-open**: bump a magic byte in the bucket header, refuse to load `master`-built buckets, and require operators to reindex from raw data. Simple to reason about; hard requirement for operators.
   - **On-open conversion**: detect the old layout by trying both parsers, rewrite the bucket on next write. Smooth rollout; doubles the parser surface and the round-trip risk.
   - Either way, **document** the path chosen in the release notes (see template below). The current Part 2 item 1 checklist leaves this open ("Provide a migration path").
2. **Are integer-indexed numeric fields silently rebuilt, or does the new code refuse them?** Item 3 changes the sortable encoding; old indexes are now wrong, not just suboptimal. Decide whether to detect-and-fail (loud) or detect-and-rebuild (silent).
3. **What does `103 "Upsert cleanup: meta missing"` do operationally?** It's a torn-write code path (item 2). Should it block the upsert, log-and-continue, or surface a metric? Pick one.

### Build + test cheatsheet

The repo builds with CMake; tests run with `ctest`. Use these after every cherry-pick:

```bash
# from repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# Filter test suite — must show 0 fail, 0 skip after item 1+4 land.
ctest --test-dir build --output-on-failure -R ndd_filter_test
ctest --test-dir build --output-on-failure -R ndd_request_validation_test

# Part-2-only suites (don't exist until item 6 lands):
ctest --test-dir build --output-on-failure -R ndd_vector_storage_test
ctest --test-dir build --output-on-failure -R ndd_numeric_index_stress_test
```

For the duplicate-cliff repro from item 1, after item 6 lands:

```bash
python3 tests/repo_filter.py     # exits non-zero on master; must exit 0 after item 1
```

### Pre-merge checklist (paste into PR description)

- [ ] All three Hypothesis tests in `filter_test.cpp` un-skipped and passing (no `GTEST_SKIP` calls remain in that file).
- [ ] `read_summary_bitmap` comment at [src/filter/numeric_index.hpp:104-140](src/filter/numeric_index.hpp#L104-L140) condensed to a one-liner about residual-byte math.
- [ ] `Bucket::is_empty()` updated to `ids.empty() && summary_bitmap.isEmpty()` ([src/filter/numeric_index.hpp:144](src/filter/numeric_index.hpp#L144)).
- [ ] `store_vectors_batch` signature gained the `is_new_to_db` parameter and every call site in [src/core/ndd.hpp](src/core/ndd.hpp) threads it.
- [ ] Six new tests in `filter_test.cpp` present and passing.
- [ ] `tests/vector_storage_test.cpp`, `tests/numeric_index_stress_test.cpp`, `tests/repo_filter.py` present.
- [ ] Migration design decision documented in the PR description and in release notes.
- [ ] Release-note draft (below) reviewed by ops / whoever runs the upgrade.

### Release-note draft

A starting template — fill in the migration choice from the open-design decisions above.

> **Filter index: breaking changes — reindex required**
>
> This release changes the on-disk format of the numeric filter bucket and the encoding used for numeric sortable keys. Filter indexes built by prior releases **cannot be read by this release** — operators must reindex all collections that use numeric filters before upgrading.
>
> - Bucket layout: the per-bucket `count` field has been removed; counts are now derived from byte length. Old buckets are detected and rejected at open time. *(or: migrated on first write — pick one.)*
> - Numeric sortable encoding: integer and float values now share a unified float32 domain. Existing integer-indexed fields will return wrong answers under the new query path; rebuild any numeric filter field that was populated under prior releases.
> - Upsert path: stale filter-index entries left behind by upserts on prior releases will not be retroactively scrubbed. A targeted rebuild is required to clear historical drift even if you do not adopt the new bucket format.
>
> Steps:
>
> 1. Stop writes to affected collections.
> 2. Drop the existing filter indexes.
> 3. Rebuild from raw vectors.
> 4. Resume writes.

### What this PR specifically does NOT touch

Keep these out of scope so reviewer focus stays on the format change:

- The header/cpp split (item 5) — already done in Part 1.
- Anything in `src/quant/`, `src/storage/` outside `vector_storage.hpp`, or `src/server/`.
- The `Carry 1` / `Carry 2` / `Carry 3` cleanups are *implicit* in items 1 and 3 — do not call them out as separate commits; they happen inside the same commit that drops the field they exist to support.
