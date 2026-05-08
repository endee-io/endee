# CMake Options

This page lists the Endee-specific CMake options exposed by `CMakeLists.txt`. Pass them when configuring the build:

```bash
cmake -S . -B build -DUSE_AVX2=ON -DND_DEBUG=ON
```

## Build Mode Options

| Option | Default | Default behavior | When enabled |
|---|---|---|---|
| `-DDEBUG=ON` | `OFF` | CMake configures a Release build, adds minimal debug symbols (`-g`), and applies `-O3 -ffast-math -fno-finite-math-only`. | Switches to a Debug build with `-O0 -g3`. Use when debugging crashes or stepping through code. |
| `-DND_DEBUG=ON` | `OFF` | `LOG_DEBUG`, `LOG_TIME`, and `PRINT_LOG_TIME` compile to no-ops. INFO/WARN/ERROR logs still emit. | Enables internal debug logs and timing summaries, currently written to the process logs/stderr. Useful for tracing behavior without a full debug build. |

`-DDEBUG=ON` and `-DND_DEBUG=ON` can be used independently. For example, `-DND_DEBUG=ON` enables internal logs while keeping the optimized Release-style build.

## SIMD Options

One SIMD option should be selected for a normal build. CMake fails if none is provided. If multiple SIMD options are set, the first matching branch in `CMakeLists.txt` is used in this order: AVX512, AVX2, SVE2, NEON.

| Option | Default | Target hardware | Compile flags and behavior |
|---|---|---|---|
| `-DUSE_AVX2=ON` | `OFF` | Modern Intel/AMD x86_64 desktops and servers. | Compiles with `-mavx2 -mfma -mf16c` and defines `USE_AVX2`. The binary checks AVX2 support at startup. |
| `-DUSE_AVX512=ON` | `OFF` | x86_64 CPUs with AVX512F, AVX512BW, AVX512VNNI, AVX512FP16, and AVX512VPOPCNTDQ. | Compiles with `-mavx512f -mavx512bw -mavx512vnni -mavx512fp16 -mavx512vpopcntdq` and defines `USE_AVX512`. The binary checks the required CPU/OS support at startup. |
| `-DUSE_NEON=ON` | `OFF` | Apple Silicon and ARMv8 NEON targets. | Defines `USE_NEON`. On Apple Silicon it uses `-mcpu=native`; on other ARM builds it uses `-march=armv8.2-a+fp16+fp16fml+dotprod`. |
| `-DUSE_SVE2=ON` | `OFF` | ARMv9/SVE2-capable servers. | Compiles with `-march=armv8.6-a+sve2+fp16+dotprod` and defines `USE_SVE2`. The binary checks SVE2 support at startup. |

The selected SIMD option also controls the output binary name:

| SIMD option | Binary name |
|---|---|
| `USE_AVX2` | `ndd-avx2` |
| `USE_AVX512` | `ndd-avx512` |
| `USE_NEON` on Linux ARM | `ndd-neon` |
| `USE_NEON` on macOS Apple Silicon | `ndd-neon-darwin` |
| `USE_SVE2` | `ndd-sve2` |

A symlink named `ndd` is created in the build directory and points to the selected binary.

## Instrumentation Options

| Option | Default | Default behavior | When enabled |
|---|---|---|---|
| `-DND_SPARSE_INSTRUMENT=ON` | `OFF` | No sparse-index timing counters are collected. | Adds sparse search/update timing counters. The health endpoint dumps and resets them through `printSparseSearchDebugStats()` and `printSparseUpdateDebugStats()`. |
| `-DND_MDBX_INSTRUMENT=ON` | `OFF` | MDBX instrumentation compiles to no-ops. | Adds timing instrumentation around MDBX read/write calls. The health endpoint dumps and resets the counters through `print_mdbx_stats()`. |

## Storage Options

| Option | Default | Default behavior | When enabled |
|---|---|---|---|
| `-DNDD_INV_IDX_STORE_FLOATS=ON` | `OFF` | Sparse index stores posting weights as quantized `uint8_t` values with a block-local max. | Stores raw `float32` posting weights instead. This avoids quantization and increases value storage from 1 byte to 4 bytes per posting. |

This setting changes sparse index storage format and memory use. Use it deliberately when you need raw sparse weights.

## Test Options

| Option | Default | Default behavior | When enabled |
|---|---|---|---|
| `-DENABLE_TESTING=ON` | `OFF` | Test targets are not generated. | Adds the `tests/` subdirectory and builds the GoogleTest-based test target. |

Example:

```bash
cmake -S . -B build-neon-tests -DUSE_NEON=ON -DENABLE_TESTING=ON
cmake --build build-neon-tests
ctest --test-dir build-neon-tests
```

## Internal Compile Macros

Some code paths are guarded by preprocessor macros that are not exposed as first-class CMake options.

| Macro | Purpose |
|---|---|
| `NDD_INV_IDX_PRUNE_DEBUG` | Tracks how many sparse posting-list entries each iterator skipped via pruning and logs the counts after search. |

Prefer the documented CMake options above for normal builds. Internal macros may be useful while developing or debugging a narrow code path, but they are not wired into `CMakeLists.txt` as `-D...=ON` options.
