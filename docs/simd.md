# SIMD Support Policy

This project requires explicit selection of a SIMD instruction set for optimized vector operations. You must enable one of the following flags during CMake configuration.

## Build Configuration

When configuring the project, you must set one of the following CMake options to `ON`. If no option is selected, the build will fail with an error identifying your architecture.

### x86_64 Options

#### 1. `USE_AVX512`
Target: Modern Intel/AMD processors (e.g., Sapphire Rapids, Zen 4/5).
**Assumptions:**
When enabled, we assume the hardware supports:
- **AVX512F** (Foundation)
- **AVX512BW** (Byte and Word instructions)
- **AVX512VNNI** (Vector Neural Network Instructions)
- **AVX512FP16** (Half-precision floating point)

#### 2. `USE_AVX2`
Target: Older x86_64 processors (Haswell and later).
**Assumptions:**
- **AVX2**
- **FMA** (Fused Multiply-Add)
- **F16C** (Float16 conversion instructions)

---

### ARM64 Options

#### 3. `USE_SVE2`
Target: ARMv9 processors (e.g., Neoverse V2, Cortex-X2).
**Assumptions:**
- **ARMv8.6-a** (Base architecture)
- **SVE2** (Scalable Vector Extension 2)
- **FP16** (Half-precision floating point)
- **INT8/INT16** dot product support

#### 4. `USE_NEON`
Target: Standard ARMv8-A processors (e.g., Apple Silicon M1/M2/M3, AWS Graviton 2).
**Assumptions:**
- **NEON**
- **FP16** (Half-precision floating point)
- **DotProd** (Dot product instructions for INT8)

## Usage Example

```bash
# For AVX512
cmake -DUSE_AVX512=ON ..

# For Apple Silicon / Standard ARM
cmake -DUSE_NEON=ON ..
```
