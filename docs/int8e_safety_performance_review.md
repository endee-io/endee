# INT8E Safety And Performance Review

Review scope: `feature/quant-int8e` compared with `master` (`git diff master...HEAD`).

Overall assessment: do not merge this as production-ready yet. The main Apple/NEON application target builds locally, but the INT8E implementation has correctness, runtime-safety, portability, readability, and unproven performance risks.

## Risk Summary

- Highest safety risks: unaligned typed reads/writes in the packed INT8E buffer and an MDBX read transaction leak if a zero-copy callback throws.
- Highest correctness risks: architecture-specific quantization behavior can diverge, non-finite input can hit undefined conversion paths, and the correction formulas have no golden tests.
- Highest performance risks: INT8E adds extra ingestion passes, scalar correction work in every distance comparison, and long-lived MDBX read transactions for zero-copy brute-force filtered search.
- The implementation must be tested separately on NEON, SVE2, AVX2, and AVX512 machines before merging.

## Problems

### 1. INT8E buffer layout uses unaligned typed pointer dereferences

- Location: `src/quant/int8e.hpp:180-190`, `src/quant/int8e.hpp:216-238`, `src/quant/int8e.hpp:607-609`, `src/quant/int8e.hpp:1344-1352`
- Severity: high
- Type: memory safety / portability

The layout is:

```text
[int8 payload | uint64_t residual-sign words | float scale]
```

For dimensions that are not multiples of 8, the residual-sign words can start at an address that is not aligned for `uint64_t`. The scale can also land on an address that is not aligned for `float`. The code then dereferences `uint64_t*` and `float*` produced from `uint8_t*`, which is undefined behavior in C++ and can fault or miscompile on stricter architectures.

Recommendation: either add explicit padding and version the layout, or keep the packed layout but read/write sign words and scale with `std::memcpy`.

### 2. `with_vectors_batch_ptrs()` leaks MDBX read transactions when the callback throws

- Location: `src/storage/vector_storage.hpp:271-297`
- Severity: high
- Type: resource safety / availability

The function opens an MDBX read transaction, invokes the callback, then aborts the transaction. If the callback throws, `mdbx_txn_abort(txn)` is skipped. The current callback allocates `fetched_ids`, `vector_ptrs`, `sims`, and heap storage in `src/core/ndd.hpp:1605-1654`, so `std::bad_alloc` alone is enough to leak the reader.

This can consume reader slots and pin old MVCC pages.

Recommendation: wrap the transaction in RAII, or use `try/catch` that always aborts before rethrowing.

### 3. Non-finite vector values can trigger undefined float-to-int conversions

- Location: `src/quant/int8e.hpp:222-225`, `src/quant/int8e.hpp:318-321`, `src/quant/int8e.hpp:408-411`, `src/quant/int8e.hpp:496-499`, `src/quant/int8e.hpp:557-560`
- Severity: high
- Type: correctness / runtime safety

The quantizers convert rounded float values to `int8_t` or SIMD integer lanes. I found no `std::isfinite` validation in the request/add path. If a vector contains `NaN`, `+inf`, `-inf`, or a value that produces an out-of-range scaled result, scalar float-to-integer conversion is undefined behavior and SIMD behavior can differ by architecture.

Recommendation: validate dense vectors before quantization and reject non-finite values with a 400-level error.

### 4. SIMD rounding and clamping behavior can diverge across server types

- Location: `src/quant/int8e.hpp:222-224`, `src/quant/int8e.hpp:283-286`, `src/quant/int8e.hpp:374-377`, `src/quant/int8e.hpp:465-468`, `src/quant/int8e.hpp:550-553`
- Severity: medium-high
- Type: cross-platform correctness

The scalar path uses `std::round`. AVX uses `_mm*_cvtps_epi32`, which follows the current floating-point rounding mode, typically ties-to-even. NEON and SVE use ARM rounding intrinsics. SVE also clamps to `[-127, 127]`, while the other paths do not explicitly clamp.

Tie cases and edge cases can produce different INT8E payloads and residual-sign bits on NEON, SVE2, AVX2, and AVX512.

Recommendation: choose one rounding and saturation rule, encode it in helper functions, and assert identical output across all SIMD builds.

### 5. INT8E correction formulas are complex and lack golden tests

- Location: `src/quant/int8e.hpp:1109-1121`, `src/quant/int8e.hpp:1293-1298`
- Severity: medium-high
- Type: correctness

`L2Sqr()` and `InnerProductSim()` use derived correction terms with constants like `0.5f`, `0.25f`, `0.125f`, and `0.0625f`. The code gives no derivation, invariant, or golden-vector tests to prove these formulas match dequantized FP32 scoring.

Recommendation: add tests comparing direct INT8E scoring against `dequantize()` plus FP32 scoring for many dimensions, metrics, value distributions, and SIMD targets.

### 6. `quantize_to_int8()` is a misleading API footgun for INT8E

- Location: `src/quant/common.hpp:75-76`, `src/quant/int8e.hpp:1344-1354`
- Severity: medium
- Type: correctness / API clarity

`QuantizerDispatch` exposes `quantize_to_int8` as a direct conversion to INT8. The INT8E implementation copies only the rotated int8 payload and scale, dropping the residual-sign bits. That is not equivalent to quantizing the original vector to INT8, because the payload is in the rotated basis and the correction data is discarded.

It appears unused today, but as a dispatch field it is easy to call later and get wrong distances.

Recommendation: either implement a true conversion through dequantize/requantize, rename it to make the lossy behavior explicit, or leave it unsupported for INT8E.

### 7. Zero-copy filtered brute force holds an MDBX read transaction open during scoring

- Location: `src/storage/vector_storage.hpp:295-297`, `src/core/ndd.hpp:1622-1654`
- Severity: medium-high
- Type: concurrency / performance

The callback computes all similarities and top-k selection while the MDBX transaction is open, because candidate pointers are only valid during that transaction. Large filtered subsets therefore become long-lived readers.

That can pin pages, delay cleanup, and make write-heavy workloads less predictable.

Recommendation: cap the brute-force path, keep the callback short, and consider copying bounded candidate bytes into owned scratch memory before expensive scoring.

### 8. `prefilter_threshold` is user-controlled without validation

- Location: `src/main.cpp:882-893`, `src/core/ndd.hpp:1588-1655`
- Severity: medium-high
- Type: performance / denial-of-service risk

The request can set `filter_params.prefilter_threshold` to any `size_t`. A high value forces brute-force scoring for large filter cardinalities. The new path avoids copying full vector bytes, but still allocates arrays proportional to filter cardinality and scores every matching candidate synchronously.

Recommendation: enforce a hard maximum, preferably tier-specific in serverless mode, and reject or clamp values above the cap.

### 9. INT8E ingestion adds extra allocations and full-vector passes

- Location: `src/quant/int8e.hpp:201-205`, `src/quant/int8e.hpp:252-256`, `src/quant/int8e.hpp:344-348`, `src/quant/int8e.hpp:433-437`, `src/quant/int8e.hpp:522-526`
- Severity: medium
- Type: ingestion performance

Every INT8E quantization path copies the input vector to `rotated`, rotates it, scans for max, and then quantizes. Compared with INT8, this adds allocation pressure and another full pass over every inserted vector.

Recommendation: benchmark add/upsert throughput and memory pressure against INT8 and INT16. If INT8E stays, consider reusable scratch buffers or fusing rotation with quantization.

### 10. INT8E distance functions add scalar bit-scanning work per comparison

- Location: `src/quant/int8e.hpp:1064-1098`, `src/quant/int8e.hpp:1256-1288`
- Severity: medium
- Type: query performance

The SIMD paths compute base dot products, but residual-sign correction scans set bits in each sign word with scalar `ctz` loops. With random-looking residual signs, this can visit roughly half the dimensions in scalar code for every vector comparison.

Recommendation: benchmark query latency at realistic dimensions and cardinalities. Consider vectorized correction or per-vector summaries if INT8E is expected to beat INT16.

### 11. INT8E batch similarity functions are not actually batch optimized

- Location: `src/quant/int8e.hpp:1314-1341`, `src/hnsw/hnswalg.h:1070-1095`
- Severity: medium
- Type: query performance

The HNSW and brute-force paths call `computeBatchSimilaritiesFromPtrs()`, but INT8E's batch functions simply loop over candidates and invoke the scalar per-vector scoring function. That improves call-site shape but does not amortize loads, correction scans, or query-side work across candidates.

Recommendation: benchmark the new path against the previous `searchKnnSubset()` path and against true batch implementations for other quantizers before claiming this is faster.

### 12. SIMD code is too duplicated and hard to review safely

- Location: `src/quant/int8e.hpp`
- Severity: medium
- Type: readability / maintainability

`int8e.hpp` is about 1,400 lines and contains separate scalar, AVX512, AVX2, NEON, and SVE2 quantize/dequantize/scoring logic. The code repeats layout math, scale writes, tail handling, residual-bit packing, and architecture-specific rounding in several places.

This makes it easy for one server type to silently drift from another.

Recommendation: split the file into shared layout/math helpers plus per-architecture kernels, and centralize common operations like scale access, sign-word access, rounding, and tail handling.

### 13. The INT8E format and math are under-documented

- Location: `src/quant/int8e.hpp:15-24`, `src/quant/int8e.hpp:193-194`, `src/quant/int8e.hpp:1109-1121`, `src/quant/int8e.hpp:1293-1298`
- Severity: medium
- Type: readability / correctness

The code introduces a new on-disk/in-memory quantized representation, a pairwise rotation, residual-sign bits, and correction formulas, but the format is only briefly described. There is no versioning note, no derivation of the correction math, and no explanation of why `0.25` is the residual center.

Recommendation: document the layout, alignment rule, math derivation, expected error bounds, and backward/forward compatibility story.

### 14. The `binary -> int8e` precision remap is intentional but should be documented

- Location: `src/main.cpp:383-390`
- Severity: low
- Type: API clarity / compatibility

Requests that set `"precision": "binary"` are rewritten to `"int8e"`. You noted this was intentional, so I am not treating it as a blocker. It is still surprising API behavior and can confuse client expectations, docs, and support/debugging.

Recommendation: document the alias clearly in API docs and release notes. Consider returning `"int8e"` in index metadata so clients can see the effective precision.

### 15. The `in8e` alias looks like a typo

- Location: `src/main.cpp:387-388`
- Severity: low
- Type: readability / API clarity

The code maps `"in8e"` to `"int8e"`. If this is meant as a compatibility alias, it should be documented. If not, it looks like a typo and makes the accepted precision surface less crisp.

Recommendation: remove it or document it as an accepted alias.

### 16. `with_vectors_batch_ptrs()` has an inconsistent failure callback shape

- Location: `src/storage/vector_storage.hpp:273-276`
- Severity: low-medium
- Type: API footgun

When `mdbx_txn_begin()` fails, the callback receives `ptrs.size() == 0` but `success.size() == count`. The current caller loops over `ptrs.size()`, so it is safe today, but the API is surprising and easy to misuse later.

Recommendation: return without invoking the callback on transaction-open failure, or pass `ptrs(count, nullptr)` and `success(count, false)`.

### 17. Test coverage does not cover the new quantizer

- Location: `tests/`
- Severity: medium-high
- Type: test coverage

There are no dedicated INT8E roundtrip, distance, cross-SIMD, persistence, filtered-search, or performance-regression tests. This is risky because most of the new logic lives in architecture-specific code paths that cannot be validated by a single local build.

Recommendation: add focused unit, integration, persistence, and benchmark coverage before merge.

## Required Checks Before Merging

### Build Matrix

- Build on NEON.
- Build on SVE2.
- Build on AVX2.
- Build on AVX512.
- Build with tests enabled on every target.
- Build with sanitizers where supported: ASAN, UBSAN, and ideally TSAN for the zero-copy/reader path.
- Confirm no SIMD target accidentally falls back to a different implementation because of compile flags.

### Unit Tests

- INT8E `get_storage_size()` for dimensions around alignment boundaries: `1`, `2`, `3`, `4`, `7`, `8`, `15`, `16`, `31`, `32`, `63`, `64`, `65`, `127`, `128`.
- Quantize/dequantize roundtrip for zero vectors, constant vectors, alternating signs, random normal values, random uniform values, very small values, and very large finite values.
- Reject `NaN`, `+inf`, and `-inf` dense values before quantization.
- Verify scalar, NEON, SVE2, AVX2, and AVX512 quantization produce identical or explicitly tolerated results.
- Verify `L2`, inner product, and cosine scoring match dequantized FP32 scoring within a documented tolerance.
- Verify odd dimensions behave correctly after pairwise rotation and inverse rotation.
- Verify residual-sign bit packing and unpacking across every partial final word case.
- Verify `quantize_to_int8()` behavior is either correct or explicitly unsupported.

### Integration Tests

- Create, save, reload, search, update, delete, and recover INT8E indexes.
- Confirm metadata reports the effective precision correctly, especially when the client sends `"binary"`.
- Run filtered search with cardinalities below, equal to, and above `prefilter_threshold`.
- Run filtered search while concurrent writes/upserts are happening.
- Run hybrid dense+sparse search with INT8E dense vectors.
- Run backup/restore for INT8E indexes and compare search results before and after restore.

### Correctness Comparisons

- Compare INT8E top-k results against FP32, INT8, and INT16 for representative datasets.
- Track recall@k, rank correlation, and score error for L2, IP, and cosine.
- Test dimensions used in production, including the maximum allowed dimension.
- Include tie-heavy vectors and near-boundary rounding vectors.

### Performance Benchmarks

- Add/upsert throughput versus INT8 and INT16.
- Query latency p50/p95/p99 versus INT8 and INT16.
- Filtered search latency across filter cardinalities.
- Memory usage per vector and per index.
- MDBX reader duration and reader-slot usage during filtered brute force.
- CPU profile of INT8E scoring to quantify residual-bit correction cost.
- Benchmark on all server types: NEON, SVE2, AVX2, and AVX512.

### Reliability Checks

- Long-running mixed workload: create, upsert, delete, filtered search, save, reload.
- Fault injection for allocation failure inside `with_vectors_batch_ptrs()` callback to prove the MDBX transaction is always aborted.
- Stress test concurrent search and write workloads with the zero-copy pointer path enabled.
- Persistence compatibility test: older indexes still load, INT8E indexes fail gracefully on binaries that do not support INT8E.

### Documentation Checks

- Document INT8E format, alignment, math, expected accuracy, and supported server types.
- Document the intentional `"binary" -> "int8e"` alias.
- Document any accepted typo/legacy aliases such as `"in8e"`, or remove them.
- Document operational guidance for `prefilter_threshold` and safe maximum values.

## Verification Notes

- `cmake --build build-apple-neon-tests` successfully built the main `ndd-neon-darwin` target on Apple/NEON.
- The full build then failed in the existing `ndd_filter_test` target:
  - `src/utils/settings.hpp` uses `std::thread` without the test target seeing `<thread>`.
  - `tests/filter_test.cpp` calls `Filter(db_path)`, but `Filter` now requires `(path, index_id)`.
- An initial configure/build using the default PATH picked the Android NDK clang and failed before project code because it could not find the standard C++ header `<algorithm>`.
- I did not verify SVE2, AVX2, or AVX512 locally.
