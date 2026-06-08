# int8e Quantization — Efficiency & Correctness Audit (working doc)

**Status:** AVX2 complete (correctness ✅, efficiency findings confirmed). NEON / SVE2 / AVX512 pending CPU access.
**File under audit:** [`src/quant/int8e.hpp`](../src/quant/int8e.hpp) (plus `find_abs_max_*` in [`src/quant/common.hpp`](../src/quant/common.hpp)).
**Branch:** `feature/quant-int8e`. **Started:** 2026-06-05.

This is the master list of efficiency changes. Workflow: (1) finalize this list across all ISAs, (2) apply the changes, (3) re-benchmark each ISA against its current baseline, (4) write a per-ISA comparison doc.

---

## 0. The int8e scheme (context for every finding)

Per-vector buffer layout (`get_storage_size`):
```
[ int8 payload (dim bytes) | residual-sign bitset (ceil(dim/64)*8 bytes) | float scale (4 bytes) ]
```
- A **pairwise butterfly rotation** `x'=(x+y)/√2, y'=(x-y)/√2` is applied before quantizing and **again** on dequant (the butterfly is its own inverse and orthonormal).
- Quantize: `scale = abs_max(rotated)/127`; `payload[i] = round(rotated[i]/scale)`; **sign bit = (residual ≥ 0)** where `residual = rotated[i]/scale − payload[i]`.
- Reconstruction (rotated domain): `(payload[i] + (signbit ? +0.25 : −0.25)) · scale`. The ±0.25 is the expected value of a uniform residual in each half-interval → ~1 extra bit of precision (measured **3.8× lower MSE** than plain int8).
- Distances (`L2Sqr`, `InnerProductSim`) are computed **in the rotated domain** from the int8 payloads plus sign-bitset corrections (a popcount term + sign-selected sums of the payload). Valid because rotation is orthonormal (L2 & inner product invariant). Math verified by hand and numerically. *Implementation note:* the main dot-product is ISA-dispatched, but the sign-selected sums are currently computed by a scalar `ctz` gather on every ISA — see F1.

---

## 1. How to reproduce (per ISA)

Harnesses are committed in [`tests/quant_audit/`](../tests/quant_audit/) (see its README). **Include `hnsw/hnswlib.h` before the quant headers** to break the `dispatch.hpp`↔`space.hpp` include cycle.

```bash
# AVX2 (this machine), run from repo root:
clang++-19 -std=c++17 -O3 -mavx2 -mfma -mf16c -DUSE_AVX2 \
  -I src -I src/utils -I third_party tests/quant_audit/int8e_verify.cpp -o /tmp/int8e_verify && /tmp/int8e_verify

# NEON (ARMv8.2+):   -march=armv8.2-a+fp16+fp16fml+dotprod -DUSE_NEON
# SVE2 (ARMv8.6+):   -march=armv8.6-a+sve2+fp16+dotprod   -DUSE_SVE2
# AVX512:            -mavx512f -mavx512bw -mavx512vnni -mavx512fp16 -mavx512vpopcntdq -DUSE_AVX512
```

Harnesses (in `tests/quant_audit/`): `int8e_verify.cpp` (correctness vs references + ground-truth double), `int8e_bench.cpp` (per-function timing + fused-dequant candidate), `int8e_dist_opt.cpp` (distance masked-sum candidate), `int8e_rot_opt.cpp` (rotation candidate). As written they target AVX2; adapt the `*_avx2` calls + candidate intrinsics per ISA.

---

## 2. Baseline measurements

Fill one column per ISA as access is granted. AVX2 = this host (AMD Zen-class, AVX2+FMA+F16C, **no** AVX-512), clang-19 -O3.

| Routine (dim=1024) | AVX2 baseline | NEON | SVE2 | AVX512 |
|---|---|---|---|---|
| `quantize_..._avx2` | 0.373 µs/vec | — | — | — |
| `dequantize_..._avx2` | 0.306 µs/vec | — | — | — |
| `L2Sqr` | 0.981 µs/pair | — | — | — |
| `InnerProductSim` | 0.981 µs/pair | — | — | — |
| `find_abs_max` | 0.078 µs/vec | — | — | — |
| `rotate` (dim=1536) | 0.262 µs | — | — | — |

---

## 3. Correctness caveats (apply across ISAs — verify per CPU)

These are not crashes; they are **consistency** issues that matter for a distributed/multi-arch DB.

### C1. Quantize rounding mode is inconsistent across ISAs ⚠️ cross-ISA
The quantize round-to-int uses a **different tie-breaking rule per backend**:

| Backend | Intrinsic | Tie rule |
|---|---|---|
| scalar | `std::round` | ties **away from zero** |
| AVX2 / AVX512 | `cvtps_epi32` (default MXCSR) | ties **to even** |
| NEON | `vcvtaq_s32_f32` | ties **away from zero** |
| SVE2 | `svrinta_f32_x` | ties **away from zero** |

So **AVX nodes disagree with NEON/SVE/scalar nodes at exact `.5` ties.** Measured on AVX2: 1 in ~2080 random vectors gets a differing payload byte (and possibly sign bit). For the *same input* this yields different stored bytes and slightly different distances depending on which CPU ingested the vector. **Decision needed:** pick one canonical rule (ties-to-even is the IEEE default and cheapest on AVX; ties-away matches current scalar/NEON/SVE). Then make all backends + the scalar tails agree. (AVX tail currently uses `std::round` while its main loop uses `cvtps_epi32` — internally inconsistent too; see F4.)

### C2. Dequant SIMD ≠ scalar in the last ULP (AVX2/AVX512/NEON)
SIMD dequant computes `payload*scale` then `+= ±0.25*scale` (3 roundings); scalar computes `(payload ± 0.25)*scale` (1 rounding). Max diff ≤ 7e-7 (≈1 ULP). **Fixed for free by F2** (the fused single-pass computes `(payload ± 0.25)*scale`, matching scalar bit-for-bit).

---

## 4. Efficiency findings — master list

Severity = perf impact. Status: ✅ confirmed (adversarially verified + benchmarked), ❓ uncertain, ❌ rejected.
"ISAs" = which backends the same issue affects (to be confirmed when those CPUs are available).

### F1 — Distance sign-bit gather is scalar `ctz`, not vectorized on ANY ISA  🔴 HIGH ✅
**Functions:** `L2Sqr` (gather at int8e.hpp:1082–1097), `InnerProductSim` (gather at int8e.hpp:1274–1287).
**ISAs: ALL — and this is NOT a scalar fallback.** These two functions are dispatched per-ISA *only for the main int8 dot-product loop*: `#if USE_AVX512 / #elif USE_AVX2 / #elif NEON / #elif SVE2 / #endif` (L2Sqr 926–1046, IP 1146–1240). After that `#endif`, the **residual-sign correction is plain C++ with no `#if` guard at all** (L2Sqr 1058–1098, IP 1250–1288), so the *same scalar block* is compiled into the AVX2, NEON, SVE2 **and** AVX512 builds. There is no `*_avx2`/`*_neon` variant of these functions and no SIMD version of the gather on any backend. Each ISA needs its own masked-sum.

> Distinguish from the **scalar tails** (`for(; i<qty)` at L2Sqr:1048, IP:1242): those are also unguarded, but only run for the `qty % width` leftover elements (negligible). F1 is different — the gather runs over the **full vector** on every build and is unconditional, not a remainder.

**Problem:** the sign-corrected sums (`sum_pos_ai_yi`, `sum_pos_bi_xi`, and for L2 also `..._ai_xi`, `..._bi_yi`) are computed by walking set bits: `__builtin_ctzll → idx=base+bit → dependent byte-load pVect[idx] → blsr` (all scalar `tzcnt`/`popcnt`/`mov`). Sign bits are ~50% set, so this is ~dim dependent-load iterations per distance call — **the dominant cost and the hottest path in HNSW search.** Mathematically each sum is just a **mask-selected horizontal sum** of contiguous int8 payload bytes.
**Fix (AVX2, verified):** for each 16-byte chunk, expand the 16 sign bits → per-byte 0x00/0xFF mask (`set1_epi16(m)` → `shuffle_epi8` spread → `and` bit-LUT → `cmpeq`), `and` with payload, `cvtepi8_epi16`, `madd(ones16)`, accumulate. Then `sum_ai_yi = 2·sum_pos − sum_yi` as today. `sum_ai_bi` (popcount XOR) is already efficient — keep it.
**Measured (AVX2, this host):** `InnerProductSim` **2.81×–3.45×** (dim 128→1536); results identical to float precision (int sums provably equal; ~1e-6 diffs are FMA-contraction noise on near-zero IPs). `L2Sqr` has 4 masked sums vs IP's 2 → expected ≥ IP's speedup.

| dim | IP orig | IP opt | speedup |
|---|---|---|---|
| 128 | 0.110 | 0.039 | 2.81× |
| 512 | 0.428 | 0.146 | 2.94× |
| 1024 | 0.989 | 0.289 | 3.43× |
| 1536 | 1.483 | 0.430 | 3.45× |

### F2 — `dequantize_..._avx2` makes two streaming passes over `out[]`  🔴 HIGH ✅
**Function:** `dequantize_int8e_buffer_to_fp32_avx2` (int8e.hpp:708–767).
**ISAs:** AVX2, AVX512 (both two-pass vectorized); NEON (two-pass, vectorized correction); SVE2 (two-pass, **scalar** correction — gets the biggest relative win).
**Problem:** Pass 1 writes `payload*scale` to all of `out[]`; Pass 2 reloads every element, adds `±0.25*scale`, stores again. The sign word is already in hand during Pass 1, so the correction can be folded in → eliminate one full read+write of `out[]`.
**Fix (AVX2, verified):** inside the dequant loop build the per-lane center via the same `srlv/and/cmpeq` mask already used in Pass 2, then store `(payload + center)*scale` once. Delete Pass 2.
**Bonus:** makes AVX2 dequant **bit-identical to scalar** (closes C2).
**Measured (AVX2, dim=1024):** 0.306 → **0.266 µs/vec (~13%)**; fused-vs-scalar max diff = **0** (bit-exact).
> Note the related rejected idea `rotate-is-third-pass`: rotation is a genuine 3rd pass but needs all elements, so it can't fold into dequant without restructuring — left as-is.

### F3 — `rotate_pairwise_inplace_avx2` uses 4 needless lane-crossing ops  🔴 HIGH ✅
**Function:** `rotate_pairwise_inplace_avx2` (int8e.hpp:69–100). Runs in **every** quantize and dequantize.
**ISAs:** **AVX2 only.** NEON uses `vld2q/vst2q` (native deinterleave — already optimal). SVE uses `svld2/svst2` (optimal). AVX512 uses `permutex2var` (different — audit separately).
**Problem:** the code deinterleaves with `shuffle` **+ `permutevar8x32`** and reinterleaves with `unpacklo/hi` **+ `permute2f128`**. The 2 `permutevar8x32` and 2 `permute2f128` (lane-crossing, port-5, ~3c) are unnecessary: the butterfly is elementwise, so it can run on the lane-shuffled order, and `unpacklo/unpackhi` put the pairs back in the correct interleaved order directly.
**Fix (verified, bit-exact):**
```cpp
__m256 lo   = _mm256_loadu_ps(&values[i]);
__m256 hi   = _mm256_loadu_ps(&values[i + 8]);
__m256 even = _mm256_shuffle_ps(lo, hi, 0x88);   // {x0,x1,x4,x5, x2,x3,x6,x7}
__m256 odd  = _mm256_shuffle_ps(lo, hi, 0xDD);   // {y0,y1,y4,y5, y2,y3,y6,y7}
__m256 nx   = _mm256_mul_ps(_mm256_add_ps(even, odd), inv_sq2);
__m256 ny   = _mm256_mul_ps(_mm256_sub_ps(even, odd), inv_sq2);
_mm256_storeu_ps(&values[i],     _mm256_unpacklo_ps(nx, ny));  // pairs 0..3, in order
_mm256_storeu_ps(&values[i + 8], _mm256_unpackhi_ps(nx, ny));  // pairs 4..7, in order
```
Remove `perm_idx` and both `permute2f128`. (The "remove only the deinterleave permute" variant was **rejected** — both must go together or ordering breaks.)
**Measured (this host):** bit-exact (0 mismatches across all dims); **1.06×–1.13×** (dim 128→1536). Workflow's llvm-mca/host run measured 1.20–1.32×. Modest but free and on a path hit by every op.

### F4 — `quantize_..._avx2` scalar tail + internal round-mode mismatch  🟡 LOW ✅
**Function:** `quantize_vector_fp32_to_int8e_buffer_avx2` tail (int8e.hpp:408–415).
**ISAs:** all SIMD backends have a `std::round` scalar tail; AVX is the only one whose **main loop** rounds differently (ties-to-even) from its tail (ties-away).
**Problem:** up to 31 scalar iterations (each: `std::round` + dependent OR into `sign_words`) for non-multiple-of-32 dims; plus the round-mode mismatch feeding C1.
**Fix:** process remaining 8-lane chunks with the main-loop sequence (`mul/cvtps_epi32/cvtepi32_ps/sub/movemask`), scalar only for the final <8 — and switch the scalar remainder to `nearbyint`/`lrintf` (or whatever canonical rule C1 picks) so the whole function is self-consistent.
**Impact:** small (tail only) but closes the internal inconsistency; do it alongside the C1 decision.

### F5 — `find_abs_max` is a separate read pass after rotation  🟠 MEDIUM ❓ (uncertain)
**Function:** quantize calls `rotate` then `find_abs_max` then the quantize loop — 3 passes over `rotated`.
**ISAs:** all.
**Idea:** accumulate abs-max inside the rotation write-back (the rotated values are live in registers), return it, skip the separate `find_abs_max` sweep.
**Verdict:** semantics fine, faster yes, but **marginal** — `find_abs_max_avx2` is only ~0.078 µs/vec and vectors are often L2-resident; requires changing the rotation signature (affects all ISAs). **Low priority** — revisit only if profiling says the abs-max pass matters.

---

## 5. Per-ISA status matrix

| Finding | AVX2 | NEON | SVE2 | AVX512 |
|---|---|---|---|---|
| F1 distance masked-sum | ✅ prototyped (3×) | ⏳ gather is scalar here too — needs NEON masked-sum | ⏳ scalar here too — needs SVE masked-sum | ⏳ scalar here too — needs AVX512 masked-sum |
| F2 dequant fusion | ✅ prototyped (13%) | ⏳ (vectorized 2-pass) | ⏳ (**scalar** 2-pass → big win) | ⏳ (2-pass) |
| F3 rotation lane ops | ✅ prototyped (1.1×) | n/a (vld2/vst2 optimal) | n/a (svld2/svst2 optimal) | ⏳ audit permutex2var path |
| F4 quantize tail/round | ✅ | ⏳ | ⏳ | ⏳ |
| F5 abs-max fuse | ❓ low-pri | ❓ | ❓ | ❓ |
| C1 rounding consistency | ⚠️ ties-to-even | ⚠️ ties-away | ⚠️ ties-away | ⚠️ ties-to-even |

---

## 6. Rejected findings (do not re-investigate)

Checked by adversarial verifiers and dismissed:
- **rotate** `redundant-deinterleave-permutevar` — rejected as a *standalone* change (breaks ordering); only valid bundled into F3.
- **rotate** `scalar-tail-up-to-7-pairs` — negligible (≤7 pairs).
- **quantize** `redundant-cmp-for-signbit` — can't take the sign directly from the float; the explicit `cmp_ps ≥ 0` is needed.
- **dequant** `blendv-to-xor-sign-flip` — XOR sign-trick not a real win over `blendv` here.
- **dequant** `redundant-broadcast-in-mask-build` — compiler already hoists/folds it.
- **L2Sqr/IP** `double-hadd-reduction`, `hadd-reduction-epilogue` — out of the hot loop (once per call).
- **L2Sqr** `accumulator-ilp-unroll` — the 5 `madd` accumulators are already independent chains (good ILP).
- **L2Sqr** `sum-a-sum-b-madd-vs-sad` — `madd(x,ones)` is fine; `sad` doesn't win for signed.
- **L2Sqr** `redundant-double-gather-fuse-loops` — superseded by F1 (the whole gather is replaced).
- **IP** `redundant-payload-reload-in-gather` — superseded by F1.
- **find_abs_max** `oversized-unroll`, `serialized-8wide-tail`, `wasted-accumulator-inits` — current 16× unroll + tree reduce is fine for typical dims.

---

## 7. Next steps

1. ⬜ Get NEON / SVE2 / AVX512 CPU access; fill §2 baselines and confirm F1/F2/F4/C1 per ISA.
2. ⬜ Decide the canonical rounding rule (C1) before touching quantize.
3. ⬜ Apply F1, F2, F3 (AVX2 prototypes ready) + per-ISA ports of F1/F2.
4. ⬜ Re-run `int8e_verify` (correctness must stay green) + benches per ISA.
5. ⬜ Write the per-ISA before/after comparison doc.

## 8. Verified AVX2 prototype sources

Committed in [`tests/quant_audit/`](../tests/quant_audit/): `int8e_verify.cpp`, `int8e_bench.cpp` (fused dequant `dequant_fused` = F2), `int8e_dist_opt.cpp` (`ip_opt` = F1), `int8e_rot_opt.cpp` (`rot_opt` = F3). Working, benchmarked implementations to port into `int8e.hpp` when we apply.
