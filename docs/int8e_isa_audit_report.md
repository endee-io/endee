# int8e Quantization — Multi-ISA Correctness & Performance Audit

**Status:** measurement complete for **AVX2, AVX-512, NEON, SVE2**. Every number below was measured on real hardware; every proposed optimization was verified bit-for-bit (or to documented float tolerance) against the production function before its speedup was recorded.

> **UPDATE:** F1+F2+F3+C1 (ties-to-even) have since been **applied** to `src/quant/int8e.hpp` and re-verified (13/13 on all ISAs) + re-benchmarked. See the before/after results in [`int8e_isa_optimization_results.md`](int8e_isa_optimization_results.md).

Companion working doc (findings rationale, rejected ideas): [`int8e_efficiency_audit.md`](int8e_efficiency_audit.md). Harnesses: [`tests/quant_audit/`](../tests/quant_audit/).

---

## 1. Executive summary

| Area | Result |
|---|---|
| **Correctness** | All int8e functions pass on all 4 ISAs (13/13 checks each). |
| **Biggest win (F1)** | The distance functions (`L2Sqr`, `InnerProductSim`) spend most of their time in a **scalar bit-gather that was never vectorized on any ISA**. Replacing it with a SIMD masked-sum: **AVX-512 up to 10.9×, NEON up to 6.2×, AVX2 up to 3.7×, SVE2 up to 3.5×.** Verified bit-exact (L2) / float-tolerance (IP). |
| **Dequant fusion (F2)** | Fuse the two-pass dequant into one pass: **SVE2 +37–41%, AVX-512 +16–36%, AMD-AVX2 +13%, NEON +2–5%, Intel-AVX2 neutral.** Also makes SIMD dequant **bit-identical to scalar** (fixes C2) on every ISA. |
| **Rotation (F3, AVX2)** | Drop 4 lane-crossing ops: **bit-exact, +10–41%** (AVX2 only). |
| **Cross-ISA correctness bug (C1)** | Quantize rounding differs by ISA: **AVX2/AVX-512 round ties-to-even, NEON/SVE2/scalar round ties-away.** Same input → different stored bytes on x86 vs ARM. Measured. |
| **SVE2 quantize anomaly** | SVE2 `quantize` is **~2× slower than NEON** because its sign-bit extraction is a scalar second pass that re-computes the scale multiply. Measured; fix not yet prototyped. |

---

## 2. Test environment

| | Server 2 (x86) | Server 1 (ARM) | Local (supplementary) |
|---|---|---|---|
| Host | `32.192.197.198` | `32.199.250.120` | this workstation |
| CPU | Intel Xeon Platinum **8488C** (Sapphire Rapids) | AWS **Neoverse-V2** (Graviton4-class) | AMD Zen-class |
| ISAs exercised | **AVX2**, **AVX-512** | **NEON** (+dotprod), **SVE2** (128-bit) | AVX2 |
| AVX-512 sub-feat | f, bw, vnni, fp16, vpopcntdq ✔ | — | — |
| ARM features | — | asimddp, sve2, i8mm, fp16, bf16 ✔ | — |
| Cores | 8 | 8 | — |
| Compiler | clang 19.1.7 | clang 19.1.7 | clang++-19 |

Build flags (from `CMakeLists.txt`):
```
AVX2    : -O3 -mavx2 -mfma -mf16c -DUSE_AVX2
AVX512  : -O3 -mavx512f -mavx512bw -mavx512vnni -mavx512fp16 -mavx512vpopcntdq -DUSE_AVX512
NEON    : -O3 -march=armv8.2-a+fp16+fp16fml+dotprod -DUSE_NEON
SVE2    : -O3 -march=armv8.6-a+sve2+fp16+dotprod -DUSE_SVE2
```
`std=c++17`, include dirs `-I src -I src/utils -I third_party`. (clang/g++ were not preinstalled on the servers; clang-19 + g++-14 were installed via apt to run this audit.)

## 3. Methodology

- **Correctness** — `int8e_verify_portable.cpp`: for each ISA build it exercises the *dispatched* (production) path over 26 dimensions (incl. odd/tail/large) × thousands of random vectors, comparing against: an ISA-aware scalar reference (payload+sign+scale), the canonical scalar dequant, and ground-truth `double` distance math. 13 assertions.
- **Baseline performance** — `int8e_bench_portable.cpp`: times the dispatched functions, min-of-5 timed batches, warmup, anti-DCE sink.
- **Candidate optimizations** — `int8e_cand_{avx2,avx512,neon,sve2}.cpp`: each contains the proposed implementation, **verifies it against the production function** (prints rel error / `FAIL`) and benchmarks baseline-vs-candidate back-to-back on the same data. A speedup is only reported when the candidate prints `OK`/`ALL VERIFIED`.
- **Rotation** — `int8e_rot_opt.cpp`: bit-exact check + timing.

Distance timings are **ns per pair** (one query vs one vector); quant/dequant/abs_max/rotate are **ns per vector**.

---

## 4. Correctness results — ✅ all ISAs pass (13/13)

| Check | AVX2 | AVX512 | NEON | SVE2 |
|---|---|---|---|---|
| find_abs_max == scalar | exact | exact | exact | exact |
| rotation == ref butterfly | bit-exact | bit-exact | bit-exact | bit-exact |
| quantize == ISA reference | exact | exact | exact | exact |
| dequant ≈ scalar | ≤7.15e-7 | ≤7.15e-7 | ≤7.15e-7 | ≤7.15e-7 |
| L2Sqr vs double | 1.8e-4 | 1.8e-4 | 1.8e-4 | 1.8e-4 |
| IPSim vs double | 1.6e-7 | 1.6e-7 | 1.6e-7 | 1.6e-7 |
| round-trip MSE vs plain int8 | 3.79× lower | 3.79× | 3.79× | 3.79× |

### C1 — Quantize rounding is inconsistent across ISAs ⚠️ (measured)
Test 3b counts vectors whose SIMD-quantized bytes differ from the scalar (`std::round`) path:

| ISA | rounding intrinsic | tie rule | vectors differing from scalar |
|---|---|---|---|
| AVX2 | `_mm256_cvtps_epi32` | ties-to-even | **1 / 2080** |
| AVX512 | `_mm512_cvtps_epi32` | ties-to-even | **1 / 2080** |
| NEON | `vcvtaq_s32_f32` | ties-away | **0 / 2080** |
| SVE2 | `svrinta_f32_x` | ties-away | **0 / 2080** |

→ x86 (ties-to-even) and ARM/scalar (ties-away) produce **different stored bytes for the same input** at `.5` ties. For a vector DB ingesting on mixed-CPU fleets this is a reproducibility hazard. Decision needed before any quantize change: pick one canonical rule. (Note AVX's *tail* uses `std::round`, so the AVX path is even internally inconsistent — see F4.)

### C2 — SIMD dequant ≠ canonical scalar in the last ULP (measured, all ISAs)
All four SIMD dequants differ from `(payload±0.25)*scale` by ≤7.15e-7 because they compute `payload*scale` then `+=0.25*scale` (3 roundings vs 1). **F2 eliminates this** — the fused dequant matched scalar bit-for-bit (`dq rel = 0.0e+00`) on every ISA.

---

## 5. Baseline performance (production code, as-is)

### Server 2 — Intel Xeon 8488C
**AVX2** (ns):
| dim | quant | dequant | L2Sqr | IPSim | abs_max | rotate |
|---|---|---|---|---|---|---|
|128|82.2|43.6|89.5|69.3|9.3|24.1|
|512|283.0|218.8|386.6|321.5|74.7|157.8|
|1024|510.6|405.3|817.3|691.2|145.1|307.5|
|1536|709.3|589.8|1224.2|1053.0|206.2|452.8|

**AVX-512** (ns):
| dim | quant | dequant | L2Sqr | IPSim | abs_max | rotate |
|---|---|---|---|---|---|---|
|128|50.3|25.4|89.6|70.1|8.1|15.3|
|512|178.6|111.6|372.4|333.1|70.5|110.6|
|1024|325.3|208.3|783.6|691.2|139.9|202.9|
|1536|428.8|258.5|1180.5|1056.6|200.7|264.5|

> **Note the smoking gun for F1:** `L2Sqr`/`IPSim` are ~identical on AVX2 vs AVX-512 (IPSim@1024 = 691.238 vs 691.245 ns) even though AVX-512 doubles SIMD width — because the scalar gather dominates and is unaffected by SIMD width.

### Server 1 — Neoverse-V2
**NEON** (ns):
| dim | quant | dequant | L2Sqr | IPSim | abs_max | rotate |
|---|---|---|---|---|---|---|
|128|112.0|94.0|126.1|108.7|13.3|37.4|
|512|346.0|353.4|517.9|461.6|52.3|125.6|
|1024|714.7|690.3|1164.2|1048.1|109.3|254.7|
|1536|1023.1|1050.3|1733.9|1562.3|164.4|381.6|

**SVE2** (ns):
| dim | quant | dequant | L2Sqr | IPSim | abs_max | rotate |
|---|---|---|---|---|---|---|
|128|185.6|96.4|126.6|107.1|16.3|39.7|
|512|648.4|363.7|523.2|468.7|71.9|144.2|
|1024|1356.6|704.6|1167.7|1063.2|169.3|290.6|
|1536|2023.4|1053.4|1750.7|1577.8|261.9|410.3|

> **SVE2 quantize anomaly:** SVE2 `quantize` (1356.6 ns @1024) is ~1.9× **slower** than NEON (714.7 ns) — its sign-bit extraction is a scalar second pass that re-computes `rotated[i]*inv_scale` per element (int8e.hpp:563–579). NEON does the sign bits in SIMD. Distances are ~equal NEON vs SVE2 (again: scalar-gather-bound).

Full 6-dim tables are in the appendix / regenerable via `int8e_bench_portable.cpp`.

---

## 6. Efficiency findings — measured per ISA

All candidate distance/dequant results below are from the `int8e_cand_*` harnesses (baseline→candidate, same data, min-of-5). **Verification:** L2Sqr and dequant matched production **bit-exactly** (rel = 0.0) on all ISAs; InnerProductSim matched to ≤2.4e-5 relative (FMA-contraction last-ULP noise on near-zero inner products — the integer sums are provably identical).

### F1 — Vectorize the distance sign-gather (🔴 HIGH, all ISAs)
Replace the scalar `ctz`/`blsr` per-set-bit gather (int8e.hpp `L2Sqr` 1082–1097, `InnerProductSim` 1274–1287 — unconditional scalar code on every build) with a SIMD masked-sum. Technique per ISA: AVX-512 `maskz_loadu_epi8` + VNNI `dpbusd`; AVX2/NEON byte-mask + `madd`/`vdotq`; SVE2 predicate-from-bitmask + `svdot`.

**InnerProductSim — ns/pair (baseline → optimized, speedup):**
| dim | AVX2 (Xeon) | AVX512 (Xeon) | NEON (V2) | SVE2 (V2) |
|---|---|---|---|---|
|128 | 66.5→29.3 (2.3×) | 70.5→15.6 (4.5×) | 108→22.4 (4.8×) | 109→34.5 (3.2×) |
|512 | 324→99.1 (3.3×) | 313→32.7 (9.6×) | 464→82.5 (5.6×) | 476→151 (3.2×) |
|1024 | 686→188 (3.6×) | 676→64.2 (10.5×) | 984→159 (6.2×) | 1019→307 (3.3×) |
|1536 | 1028→278 (3.7×) | 1001→91.6 (**10.9×**) | 1473→236 (6.2×) | 1522→466 (3.3×) |

**L2Sqr — ns/pair (speedup):**
| dim | AVX2 | AVX512 | NEON | SVE2 |
|---|---|---|---|---|
|128 | 1.9× | 3.5× | 4.5× | 3.4× |
|512 | 2.7× | 7.5× | 5.2× | 3.2× |
|1024 | 3.0× | 8.8× | 5.8× | 3.5× |
|1536 | 3.0× | **9.0×** | 5.9× | 3.4× |

AVX-512 wins biggest because the 64-bit sign word *is* a `__mmask64` — `maskz_loadu_epi8` + `dpbusd` does the masked sum with almost no overhead. SVE2 is lowest of the four because building a per-lane predicate from a bitmask (tbl + cmpne per chunk) costs more than the x86/NEON byte-mask; there is likely further headroom there.

### F2 — Fuse the two-pass dequant into one pass (🟠, all ISAs; also fixes C2)
Fold the `±0.25*scale` sign correction into the dequant pass instead of a second streaming pass; store `(payload±0.25)*scale` once.

**dequant — ns/vector (speedup), and bit-exact vs scalar:**
| dim | AVX2 (Xeon) | AVX512 (Xeon) | NEON (V2) | SVE2 (V2) | AMD-AVX2 |
|---|---|---|---|---|---|
|512 | 1.03× | 1.16× | 1.03× | 1.37× | — |
|1024 | 0.97× | 1.26× | 1.05× | 1.40× | 1.13× |
|1536 | 0.95× | 1.21× | 1.05× | 1.40× | — |

Microarch-dependent: big on SVE2 (production correction is scalar) and AVX-512; small/neutral on NEON and Intel-AVX2 (fast caches make the extra pass cheap), +13% on AMD-AVX2. **Regardless of speedup, the fused version is bit-identical to the canonical scalar dequant on every ISA** — the reason to do it is correctness (C2) as much as speed.

### F3 — Remove 4 lane-crossing ops from the AVX2 rotation (🟠, AVX2 only)
Drop the 2 `permutevar8x32` + 2 `permute2f128`; the butterfly is elementwise so `shuffle → add/sub/mul → unpacklo/hi` stores directly. **Bit-exact.**
| | dim 128 | 768 | 1536 |
|---|---|---|---|
| Xeon 8488C | 1.10× | 1.41× | 1.27× |
| AMD Zen | 1.06× | 1.09× | 1.13× |

NEON/SVE2 rotation use native `vld2/vst2`/`svld2/svst2` — already optimal, no change. AVX-512 rotation uses a different (`permutex2var`) path — not yet audited for a candidate.

### F4 — Quantize tail + rounding (🟡, all ISAs) — *characterized, fix not benchmarked*
The scalar tail (`std::round`, up to 31 elems) plus the C1 round-mode mismatch. Fix = vectorize the tail + adopt the canonical rounding rule. Low perf impact; do it together with the C1 decision.

### F5 — Fold abs-max into rotation (❓ low priority) — *not pursued*
`find_abs_max` is a separate read pass but is cheap (8–200 ns/vec). Marginal; revisit only if profiling flags it.

### New — SVE2 quantize scalar sign-pass (🟠, SVE2) — *characterized, fix not prototyped*
Root-caused above (baseline data). A fix would vectorize the sign-bit extraction (and avoid recomputing the multiply), expected to bring SVE2 quantize toward NEON's ~715 ns @1024. Not yet implemented/measured.

---

## 7. Per-ISA recommendation summary

| Finding | AVX2 | AVX512 | NEON | SVE2 |
|---|---|---|---|---|
| F1 distance masked-sum | ✅ do (≤3.7×) | ✅✅ do (≤10.9×) | ✅✅ do (≤6.2×) | ✅ do (≤3.5×) |
| F2 dequant fusion | neutral (bit-exact) | ✅ do (≤1.36×) | small (bit-exact) | ✅ do (≤1.41×) |
| F3 rotation | ✅ do (≤1.41×) | audit separately | n/a | n/a |
| F4 tail/rounding (C1) | ⚠️ decide rule | ⚠️ decide rule | ✅ matches scalar | ✅ matches scalar |
| SVE2 quant scalar pass | — | — | — | ⬜ investigate |

**Priority order to apply (after review):** F1 (huge, all ISAs) → F2 (correctness fix + SVE2/AVX-512 speed) → F3 (AVX2 free win) → C1 decision + F4 → SVE2 quantize.

---

## 8. Reproduction

On each server (code at `~/ndd_audit`, deployed via tarball; clang-19 installed):
```bash
cd ~/ndd_audit && INC="-I src -I src/utils -I third_party"
# correctness + baseline (swap flags per ISA, see §2):
clang++ -std=c++17 -O3 <ISA-FLAGS> $INC tests/quant_audit/int8e_verify_portable.cpp -o /tmp/v && /tmp/v
clang++ -std=c++17 -O3 <ISA-FLAGS> $INC tests/quant_audit/int8e_bench_portable.cpp  -o /tmp/b && /tmp/b
# candidates (verify + speedup):
clang++ -std=c++17 -O3 <ISA-FLAGS> $INC tests/quant_audit/int8e_cand_<isa>.cpp -o /tmp/c && /tmp/c
```

## 9. What I have NOT done
- Not modified `src/quant/int8e.hpp` (awaiting your review).
- Not decided the C1 canonical rounding rule (needs your call).
- Not prototyped the SVE2 quantize fix or an AVX-512 rotation candidate (characterized only).
- Candidate IP results carry ≤2.4e-5 relative FMA-noise vs production (near-zero IPs); L2 & dequant are bit-exact.
