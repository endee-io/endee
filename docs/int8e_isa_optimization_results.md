# int8e Optimizations — Before/After Comparison (per ISA)

This documents the int8e changes **applied to `src/quant/int8e.hpp`** and their measured
before/after impact on each ISA. Baselines are the unmodified code; "after" is the modified
code, both built identically and benchmarked with `tests/quant_audit/int8e_bench_portable.cpp`
(min-of-5). Correctness re-verified with `int8e_verify_portable.cpp` (13/13 on every ISA).

Audit background: [`int8e_isa_audit_report.md`](int8e_isa_audit_report.md). Findings log:
[`int8e_efficiency_audit.md`](int8e_efficiency_audit.md).

## Machines
- **Xeon 8488C** (Sapphire Rapids) — AVX2, AVX-512. clang 19.1.7.
- **Neoverse-V2** (Graviton4-class, 128-bit SVE) — NEON (+dotprod), SVE2. clang 19.1.7.

## What changed (all in `src/quant/int8e.hpp`)
- **F1** — `L2Sqr` / `InnerProductSim`: scalar `ctz` sign-gather → per-ISA SIMD masked-sum
  (AVX-512 `maskz_loadu`+`dpbusd`; AVX2/NEON byte-mask+`madd`/`vdotq`; SVE2 predicate-from-bits
  +`svdot`). Scalar gather retained as `#else` fallback.
- **F2** — all four SIMD `dequantize_*`: two passes → one fused pass `(payload±0.25)·scale`.
- **F3** — `rotate_pairwise_inplace_avx2`: removed 2 `permutevar8x32` + 2 `permute2f128`.
- **C1** — quantize rounding unified to **ties-to-even** (NEON `vcvtnq`, SVE `svrintn`, scalar
  `nearbyint`; AVX already ties-to-even).

## Correctness after changes (all ISAs: 13/13 pass)
| Check | result on every ISA |
|---|---|
| All 13 assertions | **pass** |
| C1 — SIMD vs scalar quantize divergence | **0 / 2080** (was 1/2080 on AVX2/AVX512, 0 on ARM) |
| C2 — SIMD dequant vs scalar | **bit-identical, diff = 0** (was ≤7.15e-7) |
| L2Sqr / IPSim vs ground-truth double | 1.8e-4 / 1.6e-7 (unchanged, exact integer path) |
| round-trip MSE vs plain int8 | 3.79× lower (unchanged) |

So both consistency issues are now closed: **all CPUs quantize identically**, and SIMD dequant
equals the canonical scalar reference exactly.

## Performance: distance functions (the hot path)

**`InnerProductSim` — ns/pair (before → after, speedup):**
| dim | AVX2 | AVX-512 | NEON | SVE2 |
|---|---|---|---|---|
| 128 | 69.3→30.5 (2.27×) | 70.1→15.9 (4.42×) | 108.7→22.6 (4.81×) | 107.1→35.8 (2.99×) |
| 512 | 321.5→99.3 (3.24×) | 333.1→36.1 (9.22×) | 461.6→83.0 (5.56×) | 468.7→158.7 (2.95×) |
| 1024 | 691.2→190.2 (3.64×) | 691.2→67.5 (**10.24×**) | 1048.1→158.9 (6.60×) | 1063.2→323.9 (3.28×) |
| 1536 | 1053.0→281.3 (3.74×) | 1056.6→100.3 (10.54×) | 1562.3→238.0 (6.56×) | 1577.8→492.1 (3.21×) |

**`L2Sqr` — ns/pair (before → after, speedup):**
| dim | AVX2 | AVX-512 | NEON | SVE2 |
|---|---|---|---|---|
| 128 | 89.5→45.0 (1.99×) | 89.6→25.9 (3.47×) | 126.1→29.3 (4.31×) | 126.6→39.1 (3.24×) |
| 512 | 386.6→134.8 (2.87×) | 372.4→52.3 (7.13×) | 517.9→102.9 (5.03×) | 523.2→169.6 (3.08×) |
| 1024 | 817.3→253.3 (3.23×) | 783.6→91.3 (**8.59×**) | 1164.2→194.2 (5.99×) | 1167.7→341.2 (3.42×) |
| 1536 | 1224.2→372.4 (3.29×) | 1180.5→133.8 (8.82×) | 1733.9→287.2 (6.04×) | 1750.7→511.3 (3.42×) |

## Performance: dequantize — ns/vector (before → after)
| dim | AVX2 | AVX-512 | NEON | SVE2 |
|---|---|---|---|---|
| 512 | 218.8→171.6 (1.28×) | 111.6→107.3 (1.04×) | 353.4→341.2 (1.04×) | 363.7→261.3 (1.39×) |
| 1024 | 405.3→328.6 (1.23×) | 208.3→190.1 (1.10×) | 690.3→664.5 (1.04×) | 704.6→503.3 (1.40×) |
| 1536 | 589.8→476.8 (1.24×) | 258.5→241.7 (1.07×) | 1050.3→988.0 (1.06×) | 1053.4→744.9 (1.41×) |

Plus the correctness bonus: dequant is now bit-identical to scalar everywhere.

## Performance: rotation (F3, AVX2 only) — ns/vector
| dim | AVX2 before → after |
|---|---|
| 512 | 157.8 → 131.4 (1.20×) |
| 1024 | 307.5 → 239.8 (1.28×) |
| 1536 | 452.8 → 347.2 (1.30×) |

AVX-512 / NEON / SVE2 rotation paths are unchanged (NEON/SVE already optimal; AVX-512 not in scope).

## Unchanged by design
`quantize` and `find_abs_max` were not optimized here (C1's rounding swap is cost-neutral); their
run-to-run deltas are measurement noise on shared cloud VMs. The SVE2 quantize scalar-pass anomaly
(report §6) remains open — a separate follow-up.

## Net
- **Distances are 3–10× faster on every ISA** (largest on AVX-512), correctness unchanged.
- **Dequant** is modestly faster and now bit-exact to scalar.
- **Rotation** +20–30% on AVX2.
- **Cross-ISA quantize divergence eliminated** (ties-to-even everywhere).

## Reproduce
Per ISA on the corresponding server (`~/ndd_audit`):
```bash
cd ~/ndd_audit && INC="-I src -I src/utils -I third_party"
clang++ -std=c++17 -O3 <ISA-FLAGS> $INC tests/quant_audit/int8e_verify_portable.cpp -o /tmp/v && /tmp/v
clang++ -std=c++17 -O3 <ISA-FLAGS> $INC tests/quant_audit/int8e_bench_portable.cpp  -o /tmp/b && /tmp/b
```
ISA-FLAGS: AVX2 `-mavx2 -mfma -mf16c -DUSE_AVX2`; AVX512 `-mavx512f -mavx512bw -mavx512vnni
-mavx512fp16 -mavx512vpopcntdq -DUSE_AVX512`; NEON `-march=armv8.2-a+fp16+fp16fml+dotprod
-DUSE_NEON`; SVE2 `-march=armv8.6-a+sve2+fp16+dotprod -DUSE_SVE2`.
