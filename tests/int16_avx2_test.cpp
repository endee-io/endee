/**
 * Accuracy + performance tests for the AVX2 int16 quantization path added in
 * the port of endee-io/endee#228. These tests only build when USE_AVX2 is
 * defined and the host CPU supports AVX2/FMA. They exercise three claims:
 *
 *   1. The new AVX2 quantize/dequantize functions are bit-equivalent (or within
 *      one ULP of the per-vector scale) to the scalar reference implementation.
 *   2. The rewritten AVX2 dot-product loop (madd_epi16, stride 16) produces the
 *      same value as both the scalar reference and the pre-PR AVX2 loop
 *      (mullo_epi32, stride 8) on a large randomized corpus.
 *   3. The new AVX2 paths are measurably faster than (a) scalar baselines and
 *      (b) the pre-PR AVX2 dot product loop.
 */

#include <gtest/gtest.h>

#if !defined(USE_AVX2)
TEST(Int16Avx2, BuildGuard) {
    GTEST_SKIP() << "Test only meaningful when built with -DUSE_AVX2=ON";
}
#else

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <random>
#include <vector>

#include "quant/int16.hpp"

namespace {

using ndd::quant::int16::dequantize_int16_buffer_to_fp32;
using ndd::quant::int16::dequantize_int16_buffer_to_fp32_avx2;
using ndd::quant::int16::extract_scale;
using ndd::quant::int16::get_storage_size;
using ndd::quant::int16::INT16_SCALE;
using ndd::quant::int16::InnerProductSimBatch;
using ndd::quant::int16::L2SqrSimBatch;
using ndd::quant::int16::quantize_vector_fp32_to_int16_buffer;
using ndd::quant::int16::quantize_vector_fp32_to_int16_buffer_avx2;

constexpr int kSeed = 0x5eed;

std::vector<float> make_random_vector(size_t dim, float lo, float hi, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(lo, hi);
    std::vector<float> v(dim);
    for(size_t i = 0; i < dim; ++i) v[i] = dist(rng);
    return v;
}

/** Scalar reference dequantize — bit-equivalent to the dispatcher's #else
 * branch. Used as the accuracy baseline. */
std::vector<float> dequantize_scalar(const uint8_t* buffer, size_t dim) {
    std::vector<float> out(dim);
    const int16_t* data = reinterpret_cast<const int16_t*>(buffer);
    float scale = extract_scale(buffer, dim);
    for(size_t i = 0; i < dim; ++i) {
        out[i] = static_cast<float>(data[i]) * scale;
    }
    return out;
}

/** Truly-scalar dequantize for the performance comparison. The plain loop
 * above is auto-vectorized to AVX2 at -O3, so it cannot serve as a meaningful
 * "scalar baseline" for a perf assertion. The pragma below disables loop
 * vectorization (Clang) and the noinline attribute keeps the function from
 * being merged with its caller. */
__attribute__((noinline))
std::vector<float> dequantize_scalar_unvectorized(const uint8_t* buffer, size_t dim) {
    std::vector<float> out(dim);
    const int16_t* data = reinterpret_cast<const int16_t*>(buffer);
    float scale = extract_scale(buffer, dim);
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable)
#elif defined(__GNUC__)
#pragma GCC novector
#endif
    for(size_t i = 0; i < dim; ++i) {
        out[i] = static_cast<float>(data[i]) * scale;
    }
    return out;
}

/** Pre-PR AVX2 dot product loop (stride 8, mullo_epi32). Used only by the
 * performance test to demonstrate the speedup from the madd_epi16 rewrite.
 * The `asm volatile("" ::: "memory")` barrier prevents Clang's IPA from
 * marking this as a pure function and CSEing the call across iterations. */
__attribute__((noinline))
int64_t pre_pr_avx2_dot(const int16_t* a, const int16_t* b, size_t n, bool l2,
                        int64_t* out_sq) {
    asm volatile("" ::: "memory");
    __m256i dot_lo = _mm256_setzero_si256();
    __m256i dot_hi = _mm256_setzero_si256();
    __m256i sq_lo = _mm256_setzero_si256();
    __m256i sq_hi = _mm256_setzero_si256();
    size_t d = 0;
    for(; d + 8 <= n; d += 8) {
        __m128i q_i16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + d));
        __m128i v_i16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + d));
        __m256i q_i32 = _mm256_cvtepi16_epi32(q_i16);
        __m256i v_i32 = _mm256_cvtepi16_epi32(v_i16);
        __m256i dot_i32 = _mm256_mullo_epi32(q_i32, v_i32);
        dot_lo = _mm256_add_epi64(dot_lo,
            _mm256_cvtepi32_epi64(_mm256_castsi256_si128(dot_i32)));
        dot_hi = _mm256_add_epi64(dot_hi,
            _mm256_cvtepi32_epi64(_mm256_extracti128_si256(dot_i32, 1)));
        if(l2) {
            __m256i sq_i32 = _mm256_mullo_epi32(v_i32, v_i32);
            sq_lo = _mm256_add_epi64(sq_lo,
                _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sq_i32)));
            sq_hi = _mm256_add_epi64(sq_hi,
                _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sq_i32, 1)));
        }
    }
    int64_t dot = 0;
    {
        __m128i d_lo = _mm_add_epi64(_mm256_castsi256_si128(dot_lo),
                                     _mm256_extracti128_si256(dot_lo, 1));
        __m128i d_hi = _mm_add_epi64(_mm256_castsi256_si128(dot_hi),
                                     _mm256_extracti128_si256(dot_hi, 1));
        d_lo = _mm_add_epi64(d_lo, d_hi);
        d_lo = _mm_add_epi64(d_lo, _mm_unpackhi_epi64(d_lo, d_lo));
        dot = static_cast<int64_t>(_mm_cvtsi128_si64(d_lo));
    }
    if(l2 && out_sq) {
        __m128i s_lo = _mm_add_epi64(_mm256_castsi256_si128(sq_lo),
                                     _mm256_extracti128_si256(sq_lo, 1));
        __m128i s_hi = _mm_add_epi64(_mm256_castsi256_si128(sq_hi),
                                     _mm256_extracti128_si256(sq_hi, 1));
        s_lo = _mm_add_epi64(s_lo, s_hi);
        s_lo = _mm_add_epi64(s_lo, _mm_unpackhi_epi64(s_lo, s_lo));
        *out_sq = static_cast<int64_t>(_mm_cvtsi128_si64(s_lo));
    }
    for(; d < n; ++d) {
        dot += static_cast<int64_t>(a[d]) * b[d];
        if(l2 && out_sq) *out_sq += static_cast<int64_t>(b[d]) * b[d];
    }
    return dot;
}

/** Scalar reference dot product against which both AVX2 variants are checked. */
int64_t scalar_dot(const int16_t* a, const int16_t* b, size_t n) {
    int64_t s = 0;
    for(size_t i = 0; i < n; ++i) s += static_cast<int64_t>(a[i]) * b[i];
    return s;
}

template <typename F>
double time_ns(F&& fn, int iters) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for(int i = 0; i < iters; ++i) fn();
    auto t1 = clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
           / static_cast<double>(iters);
}

}  // namespace

/* =====================================================================
 * Accuracy
 * ===================================================================== */

TEST(Int16Avx2Accuracy, QuantizeMatchesScalar) {
    for(size_t dim : {17u, 64u, 128u, 256u, 1023u, 1024u, 1537u}) {
        auto input = make_random_vector(dim, -3.5f, 3.5f, kSeed + dim);
        auto buf_scalar = quantize_vector_fp32_to_int16_buffer(input);
        auto buf_avx2 = quantize_vector_fp32_to_int16_buffer_avx2(input);
        ASSERT_EQ(buf_scalar.size(), buf_avx2.size()) << "dim=" << dim;

        // Scale (last 4 bytes) must be bit-identical.
        EXPECT_EQ(extract_scale(buf_scalar.data(), dim),
                  extract_scale(buf_avx2.data(), dim)) << "dim=" << dim;

        // Per-element values may differ by at most 1 ULP due to the conversion
        // path: scalar uses std::round; _mm256_cvtps_epi32 uses MXCSR round-to-
        // nearest (banker's rounding). Both rules agree on every input except
        // exact half-integers, which are vanishingly rare for random floats.
        const int16_t* s = reinterpret_cast<const int16_t*>(buf_scalar.data());
        const int16_t* v = reinterpret_cast<const int16_t*>(buf_avx2.data());
        size_t mismatches = 0;
        for(size_t i = 0; i < dim; ++i) {
            int diff = std::abs(static_cast<int>(s[i]) - static_cast<int>(v[i]));
            EXPECT_LE(diff, 1) << "dim=" << dim << " i=" << i
                               << " scalar=" << s[i] << " avx2=" << v[i];
            if(diff != 0) ++mismatches;
        }
        EXPECT_LT(mismatches, dim / 10) << "dim=" << dim
            << " — too many off-by-1 rounding mismatches";
    }
}

TEST(Int16Avx2Accuracy, DequantizeMatchesScalar) {
    for(size_t dim : {17u, 64u, 128u, 256u, 1023u, 1024u, 1537u}) {
        auto input = make_random_vector(dim, -2.0f, 2.0f, kSeed + dim * 7);
        auto buf = quantize_vector_fp32_to_int16_buffer(input);

        auto out_scalar = dequantize_scalar(buf.data(), dim);
        auto out_avx2 = dequantize_int16_buffer_to_fp32_avx2(buf.data(), dim);

        ASSERT_EQ(out_scalar.size(), out_avx2.size()) << "dim=" << dim;
        for(size_t i = 0; i < dim; ++i) {
            EXPECT_FLOAT_EQ(out_scalar[i], out_avx2[i]) << "dim=" << dim << " i=" << i;
        }
    }
}

TEST(Int16Avx2Accuracy, RoundTripBoundedError) {
    const size_t dim = 1024;
    auto input = make_random_vector(dim, -1.0f, 1.0f, kSeed);
    auto buf = quantize_vector_fp32_to_int16_buffer_avx2(input);
    auto out = dequantize_int16_buffer_to_fp32_avx2(buf.data(), dim);

    float abs_max = 0.0f;
    for(float x : input) abs_max = std::max(abs_max, std::fabs(x));
    const float scale = abs_max / INT16_SCALE;
    const float tol = scale * 1.001f;  // one quantization step + tiny slack

    for(size_t i = 0; i < dim; ++i) {
        EXPECT_NEAR(input[i], out[i], tol) << "i=" << i;
    }
}

TEST(Int16Avx2Accuracy, DotProductLoopMatchesScalarAndPrePr) {
    // Pick lengths that exercise both the SIMD stride and the scalar tail
    // (lengths 8 .. 15 now fall to the scalar tail with the new stride-16 loop).
    for(size_t n : {16u, 17u, 31u, 32u, 47u, 64u, 100u, 256u, 1023u}) {
        std::mt19937 rng(kSeed + n);
        std::uniform_int_distribution<int> d(-30000, 30000);
        std::vector<int16_t> a(n), b(n);
        for(size_t i = 0; i < n; ++i) {
            a[i] = static_cast<int16_t>(d(rng));
            b[i] = static_cast<int16_t>(d(rng));
        }

        int64_t want = scalar_dot(a.data(), b.data(), n);
        int64_t pre_sq = 0;
        int64_t pre = pre_pr_avx2_dot(a.data(), b.data(), n, /*l2=*/true, &pre_sq);
        EXPECT_EQ(pre, want) << "n=" << n;

        // Exercise the in-tree (new) AVX2 loop through SimilarityBatchTiled.
        // Layout each "vector" as a quantized int16 buffer with a trailing scale=1.
        std::vector<uint8_t> qa(get_storage_size(n));
        std::vector<uint8_t> qb(get_storage_size(n));
        std::memcpy(qa.data(), a.data(), n * sizeof(int16_t));
        std::memcpy(qb.data(), b.data(), n * sizeof(int16_t));
        const float one = 1.0f;
        std::memcpy(qa.data() + n * sizeof(int16_t), &one, sizeof(float));
        std::memcpy(qb.data() + n * sizeof(int16_t), &one, sizeof(float));

        hnswlib::DistParams params{n, /*quant_level=*/2};
        const void* vec_ptrs[1] = {qb.data()};
        float out = 0.0f;

        // InnerProductSim returns float(dot * scale_a * scale_b). With both
        // scales = 1 it equals float(want). Tolerance must follow float
        // precision at the answer's magnitude — for random ±30k int16 over
        // n=1024 elements the dot can reach ~10^10, where one float ULP is
        // ~10^3. Use a relative tolerance.
        InnerProductSimBatch(qa.data(), vec_ptrs, 1, &params, &out);
        const double ip_tol = std::max(2.0, std::fabs(static_cast<double>(want)) * 1e-6);
        EXPECT_NEAR(static_cast<double>(out), static_cast<double>(want), ip_tol)
            << "n=" << n;

        // L2SqrSim returns -float(|a|² + |b|² - 2·a·b). Reconstruct dot from
        // it; tolerance is dominated by the largest single term (|a|² or |b|²),
        // each truncated to float precision before the int64→float cast.
        float l2_neg = 0.0f;
        L2SqrSimBatch(qa.data(), vec_ptrs, 1, &params, &l2_neg);
        const double a_sq = static_cast<double>(scalar_dot(a.data(), a.data(), n));
        const double b_sq = static_cast<double>(scalar_dot(b.data(), b.data(), n));
        const double reconstructed_dot = (a_sq + b_sq - (-l2_neg)) / 2.0;
        const double l2_tol = std::max(4.0,
            std::max({std::fabs(a_sq), std::fabs(b_sq), std::fabs(static_cast<double>(want))})
            * 1e-5);
        EXPECT_NEAR(reconstructed_dot, static_cast<double>(want), l2_tol)
            << "n=" << n;
    }
}

/* =====================================================================
 * Performance — these tests assert a minimum speedup but log timings even
 * when they pass. If a noisy CI host produces a flake, lower the floor; the
 * intent is "AVX2 is meaningfully faster", not "AVX2 is exactly Nx faster".
 * ===================================================================== */

TEST(Int16Avx2Performance, QuantizeBeatsScalar) {
    const size_t dim = 1024;
    const int iters = 5000;
    auto input = make_random_vector(dim, -1.0f, 1.0f, kSeed);

    double ns_scalar = time_ns(
        [&] { auto b = quantize_vector_fp32_to_int16_buffer(input); (void)b; },
        iters);
    double ns_avx2 = time_ns(
        [&] { auto b = quantize_vector_fp32_to_int16_buffer_avx2(input); (void)b; },
        iters);

    std::cerr << "[perf] quantize  dim=" << dim
              << "  scalar=" << ns_scalar << " ns/iter"
              << "  avx2=" << ns_avx2 << " ns/iter"
              << "  speedup=" << (ns_scalar / ns_avx2) << "x\n";
    EXPECT_LT(ns_avx2, ns_scalar)
        << "AVX2 quantize should be faster than scalar";
}

TEST(Int16Avx2Performance, DequantizeBeatsScalar) {
    const size_t dim = 1024;
    const int iters = 5000;
    auto input = make_random_vector(dim, -1.0f, 1.0f, kSeed);
    auto buf = quantize_vector_fp32_to_int16_buffer(input);

    // Use the no-vectorize scalar variant — at -O3 the plain loop is auto-
    // vectorized to AVX2 by the compiler, which would defeat this perf test.
    double ns_scalar = time_ns(
        [&] {
            auto o = dequantize_scalar_unvectorized(buf.data(), dim);
            volatile float sink = o.empty() ? 0.0f : o.back();
            (void)sink;
        },
        iters);
    double ns_avx2 = time_ns(
        [&] {
            auto o = dequantize_int16_buffer_to_fp32_avx2(buf.data(), dim);
            volatile float sink = o.empty() ? 0.0f : o.back();
            (void)sink;
        },
        iters);

    std::cerr << "[perf] dequant   dim=" << dim
              << "  scalar=" << ns_scalar << " ns/iter"
              << "  avx2=" << ns_avx2 << " ns/iter"
              << "  speedup=" << (ns_scalar / ns_avx2) << "x\n";
    EXPECT_LT(ns_avx2, ns_scalar)
        << "AVX2 dequantize should be faster than truly-scalar dequantize";
}

/** New (in-tree) AVX2 dot product as a noinline function so the compiler
 * cannot hoist a loop-invariant computation out of the per-iteration timing
 * harness. */
__attribute__((noinline))
int64_t new_avx2_dot(const int16_t* a, const int16_t* b, size_t n) {
    asm volatile("" ::: "memory");
    __m256i dot_lo = _mm256_setzero_si256();
    __m256i dot_hi = _mm256_setzero_si256();
    size_t k = 0;
    for(; k + 16 <= n; k += 16) {
        __m256i q = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + k));
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + k));
        __m256i dot_i32 = _mm256_madd_epi16(q, v);
        dot_lo = _mm256_add_epi64(dot_lo,
            _mm256_cvtepi32_epi64(_mm256_castsi256_si128(dot_i32)));
        dot_hi = _mm256_add_epi64(dot_hi,
            _mm256_cvtepi32_epi64(_mm256_extracti128_si256(dot_i32, 1)));
    }
    __m128i lo = _mm_add_epi64(_mm256_castsi256_si128(dot_lo),
                               _mm256_extracti128_si256(dot_lo, 1));
    __m128i hi = _mm_add_epi64(_mm256_castsi256_si128(dot_hi),
                               _mm256_extracti128_si256(dot_hi, 1));
    lo = _mm_add_epi64(lo, hi);
    lo = _mm_add_epi64(lo, _mm_unpackhi_epi64(lo, lo));
    int64_t result = static_cast<int64_t>(_mm_cvtsi128_si64(lo));
    for(; k < n; ++k) result += static_cast<int64_t>(a[k]) * b[k];
    return result;
}

TEST(Int16Avx2Performance, DotProductBeatsPrePrLoop) {
    // Run the inner loop in isolation (no malloc/dispatch overhead) so the
    // measurement reflects the actual SIMD body that changed.
    const size_t n = 256;  // matches kBatchTileSizeInt16 for AVX2
    const int iters = 200000;
    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<int> d(-30000, 30000);
    std::vector<int16_t> a(n), b(n);
    for(size_t i = 0; i < n; ++i) {
        a[i] = static_cast<int16_t>(d(rng));
        b[i] = static_cast<int16_t>(d(rng));
    }

    // Sanity: both produce the same answer before timing.
    ASSERT_EQ(new_avx2_dot(a.data(), b.data(), n),
              pre_pr_avx2_dot(a.data(), b.data(), n, false, nullptr));

    auto new_loop = [&] {
        volatile int64_t sink = new_avx2_dot(a.data(), b.data(), n);
        (void)sink;
    };
    auto old_loop = [&] {
        volatile int64_t sink = pre_pr_avx2_dot(a.data(), b.data(), n,
                                                /*l2=*/false, /*out_sq=*/nullptr);
        (void)sink;
    };

    double ns_old = time_ns(old_loop, iters);
    double ns_new = time_ns(new_loop, iters);
    std::cerr << "[perf] dot loop  n=" << n
              << "  pre_pr_avx2=" << ns_old << " ns/iter"
              << "  new_avx2=" << ns_new << " ns/iter"
              << "  speedup=" << (ns_old / ns_new) << "x\n";
    EXPECT_LT(ns_new, ns_old)
        << "New AVX2 dot loop (madd_epi16, stride 16) should be faster than "
           "the pre-PR loop (mullo_epi32, stride 8)";
}

#endif  // USE_AVX2
