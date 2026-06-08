#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <memory>
#include <cstring>
#include "common.hpp"
#include "int8.hpp"
#include "../hnsw/hnswlib.h"

namespace ndd {
    namespace quant {
        namespace int8e {

            constexpr float INT8_SCALE = 127.0f;
            constexpr float INV_SQRT2 = 0.7071067811865475f;

            constexpr size_t get_sign_word_count(size_t dimension) {
                return (dimension + 63) / 64;
            }

            constexpr size_t get_sign_storage_size(size_t dimension) {
                return get_sign_word_count(dimension) * sizeof(uint64_t);
            }

// ─────────────────────────────────────────────────────────────────────────────
// Helpers for the vectorised residual-sign masked-sum (distance functions).
// Each expands a packed sign bitmask into a per-lane select mask/predicate.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(USE_AVX2)
            // 16-bit lane mask -> 16 bytes of 0xFF/0x00 (byte i = 0xFF iff bit i set)
            static inline __m128i int8e_mask16_to_bytes(uint16_t m) {
                const __m128i shuf = _mm_setr_epi8(0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1);
                const __m128i bit  = _mm_setr_epi8(1,2,4,8,16,32,64,(char)128, 1,2,4,8,16,32,64,(char)128);
                __m128i v = _mm_set1_epi16((short)m);
                v = _mm_shuffle_epi8(v, shuf);
                v = _mm_and_si128(v, bit);
                return _mm_cmpeq_epi8(v, bit);
            }
            static inline int64_t int8e_hsum_epi32_avx2(__m256i v) {
                __m128i lo = _mm256_castsi256_si128(v);
                __m128i hi = _mm256_extracti128_si256(v, 1);
                __m128i s  = _mm_add_epi32(lo, hi);
                s = _mm_hadd_epi32(s, s);
                s = _mm_hadd_epi32(s, s);
                return static_cast<int64_t>(_mm_cvtsi128_si32(s));
            }
#endif
#if defined(USE_NEON)
            // 16-bit lane mask -> 16 bytes of 0xFF/0x00
            static inline int8x16_t int8e_mask16_to_bytes_neon(uint16_t bits) {
                uint8x16_t v = vcombine_u8(vdup_n_u8((uint8_t)(bits & 0xFF)),
                                           vdup_n_u8((uint8_t)(bits >> 8)));
                static const uint8_t bp[16] = {1,2,4,8,16,32,64,128, 1,2,4,8,16,32,64,128};
                uint8x16_t bit = vld1q_u8(bp);
                return vreinterpretq_s8_u8(vceqq_u8(vandq_u8(v, bit), bit));
            }
#endif
#if defined(USE_SVE2)
            // Byte predicate: lane i active iff bit i of chunkbits set, restricted to pg.
            static inline svbool_t int8e_pred_from_bits(svbool_t pg, uint64_t chunkbits) {
                uint8_t cb[64];
                std::memset(cb, 0, sizeof(cb));
                std::memcpy(cb, &chunkbits, 8);
                svuint8_t mbytes  = svld1_u8(svptrue_b8(), cb);
                svuint8_t lane    = svindex_u8(0, 1);
                svuint8_t byteidx = svlsr_n_u8_x(svptrue_b8(), lane, 3);            // i/8
                svuint8_t bitpos  = svlsl_u8_x(svptrue_b8(), svdup_u8(1),
                                               svand_n_u8_x(svptrue_b8(), lane, 7)); // 1<<(i&7)
                svuint8_t spread  = svtbl_u8(mbytes, byteidx);
                return svcmpne_n_u8(pg, svand_u8_x(svptrue_b8(), spread, bitpos), 0);
            }
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Pairwise butterfly rotation: x'=(x+y)*INV_SQRT2, y'=(x-y)*INV_SQRT2
// Used during quantize and dequantize on pairs of adjacent elements.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(USE_AVX512)
            // AVX512: 16 pairs (32 floats) per iteration using _mm512_permutex2var_ps
            inline void rotate_pairwise_inplace_avx512(std::vector<float>& values) {
                const size_t dimension = values.size();
                const size_t paired    = (dimension / 2) * 2;
                const __m512 inv_sq2   = _mm512_set1_ps(INV_SQRT2);
                // Gather even-indexed (x) and odd-indexed (y) elements from two 16-float loads
                const __m512i even_idx =
                        _mm512_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30);
                const __m512i odd_idx =
                        _mm512_setr_epi32(1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31);
                // Scatter new_x and new_y back into interleaved pairs
                const __m512i lo_idx =
                        _mm512_setr_epi32(0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
                const __m512i hi_idx =
                        _mm512_setr_epi32(8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);

                size_t i = 0;
                for(; i + 32 <= paired; i += 32) {
                    __m512 lo   = _mm512_loadu_ps(&values[i]);
                    __m512 hi   = _mm512_loadu_ps(&values[i + 16]);
                    __m512 xv   = _mm512_permutex2var_ps(lo, even_idx, hi);
                    __m512 yv   = _mm512_permutex2var_ps(lo, odd_idx, hi);
                    __m512 nx   = _mm512_mul_ps(_mm512_add_ps(xv, yv), inv_sq2);
                    __m512 ny   = _mm512_mul_ps(_mm512_sub_ps(xv, yv), inv_sq2);
                    _mm512_storeu_ps(&values[i],      _mm512_permutex2var_ps(nx, lo_idx, ny));
                    _mm512_storeu_ps(&values[i + 16], _mm512_permutex2var_ps(nx, hi_idx, ny));
                }
                for(; i < paired; i += 2) {
                    const float x = values[i], y = values[i + 1];
                    values[i]     = (x + y) * INV_SQRT2;
                    values[i + 1] = (x - y) * INV_SQRT2;
                }
            }
#endif

#if defined(USE_AVX2)
            // AVX2: 8 pairs (16 floats) per iteration. Deinterleave + butterfly + reinterleave
            // using only per-128-bit-lane shuffles/unpacks — no cross-lane permutes.
            inline void rotate_pairwise_inplace_avx2(std::vector<float>& values) {
                const size_t dimension = values.size();
                const size_t paired    = (dimension / 2) * 2;
                const __m256  inv_sq2  = _mm256_set1_ps(INV_SQRT2);

                size_t i = 0;
                for(; i + 16 <= paired; i += 16) {
                    // lo=[x0,y0,x1,y1, x2,y2,x3,y3], hi=[x4,y4,x5,y5, x6,y6,x7,y7]
                    __m256 lo   = _mm256_loadu_ps(&values[i]);
                    __m256 hi   = _mm256_loadu_ps(&values[i + 8]);
                    // Deinterleave within each 128-bit lane (no cross-lane permute needed):
                    __m256 even = _mm256_shuffle_ps(lo, hi, 0x88);  // {x0,x1,x4,x5, x2,x3,x6,x7}
                    __m256 odd  = _mm256_shuffle_ps(lo, hi, 0xDD);  // {y0,y1,y4,y5, y2,y3,y6,y7}
                    // Butterfly is element-wise, so it commutes with the lane-shuffled order.
                    __m256 nx   = _mm256_mul_ps(_mm256_add_ps(even, odd), inv_sq2);
                    __m256 ny   = _mm256_mul_ps(_mm256_sub_ps(even, odd), inv_sq2);
                    // unpacklo/unpackhi reinterleave the pairs back in order, per 128-bit lane,
                    // landing pairs 0..3 in values[i..i+7] and 4..7 in values[i+8..i+15] directly.
                    _mm256_storeu_ps(&values[i],     _mm256_unpacklo_ps(nx, ny));
                    _mm256_storeu_ps(&values[i + 8], _mm256_unpackhi_ps(nx, ny));
                }
                for(; i < paired; i += 2) {
                    const float x = values[i], y = values[i + 1];
                    values[i]     = (x + y) * INV_SQRT2;
                    values[i + 1] = (x - y) * INV_SQRT2;
                }
            }
#endif

#if defined(USE_SVE2)
            // SVE2: variable-width using svld2/svst2 which deinterleave/reinterleave natively
            inline void rotate_pairwise_inplace_sve(std::vector<float>& values) {
                const size_t dimension = values.size();
                const size_t paired    = (dimension / 2) * 2;
                const size_t vl        = svcntw();   // float32 elements per SVE register
                const size_t step      = vl * 2;     // process vl pairs at a time

                size_t i = 0;
                for(; i + step <= paired; i += step) {
                    svfloat32x2_t pairs = svld2_f32(svptrue_b32(), &values[i]);
                    svfloat32_t   xv    = svget2_f32(pairs, 0);
                    svfloat32_t   yv    = svget2_f32(pairs, 1);
                    svfloat32_t   nx    = svmul_n_f32_x(
                            svptrue_b32(), svadd_f32_x(svptrue_b32(), xv, yv), INV_SQRT2);
                    svfloat32_t   ny    = svmul_n_f32_x(
                            svptrue_b32(), svsub_f32_x(svptrue_b32(), xv, yv), INV_SQRT2);
                    svst2_f32(svptrue_b32(), &values[i], svcreate2_f32(nx, ny));
                }
                for(; i < paired; i += 2) {
                    const float x = values[i], y = values[i + 1];
                    values[i]     = (x + y) * INV_SQRT2;
                    values[i + 1] = (x - y) * INV_SQRT2;
                }
            }
#endif

#if defined(USE_NEON)
            // NEON: 4 pairs (8 floats) per iteration; vld2q/vst2q handle deinterleave/reinterleave
            inline void rotate_pairwise_inplace_neon(std::vector<float>& values) {
                const size_t dimension = values.size();
                const size_t paired    = (dimension / 2) * 2;
                const float32x4_t inv_sq2 = vdupq_n_f32(INV_SQRT2);

                size_t i = 0;
                for(; i + 8 <= paired; i += 8) {
                    float32x4x2_t pairs = vld2q_f32(&values[i]);
                    float32x4_t   nx    = vmulq_f32(vaddq_f32(pairs.val[0], pairs.val[1]), inv_sq2);
                    float32x4_t   ny    = vmulq_f32(vsubq_f32(pairs.val[0], pairs.val[1]), inv_sq2);
                    float32x4x2_t result = {{nx, ny}};
                    vst2q_f32(&values[i], result);
                }
                for(; i < paired; i += 2) {
                    const float x = values[i], y = values[i + 1];
                    values[i]     = (x + y) * INV_SQRT2;
                    values[i + 1] = (x - y) * INV_SQRT2;
                }
            }
#endif

            // Dispatch to best available implementation
            inline void rotate_pairwise_inplace(std::vector<float>& values) {
#if defined(USE_AVX512)
                rotate_pairwise_inplace_avx512(values);
#elif defined(USE_AVX2)
                rotate_pairwise_inplace_avx2(values);
#elif defined(USE_SVE2)
                rotate_pairwise_inplace_sve(values);
#elif defined(USE_NEON)
                rotate_pairwise_inplace_neon(values);
#else
                const size_t dimension = values.size();
                const size_t paired    = (dimension / 2) * 2;
                for(size_t i = 0; i < paired; i += 2) {
                    const float x = values[i];
                    const float y = values[i + 1];
                    values[i]     = (x + y) * INV_SQRT2;
                    values[i + 1] = (x - y) * INV_SQRT2;
                }
#endif
            }

            constexpr size_t get_storage_size(size_t dimension) {
                return dimension * sizeof(int8_t) + get_sign_storage_size(dimension)
                       + sizeof(float);
            }

            inline const uint64_t* extract_sign_words(const uint8_t* buffer, size_t dimension) {
                return reinterpret_cast<const uint64_t*>(buffer + dimension * sizeof(int8_t));
            }

            inline uint64_t* extract_sign_words(uint8_t* buffer, size_t dimension) {
                return reinterpret_cast<uint64_t*>(buffer + dimension * sizeof(int8_t));
            }

            inline float extract_scale(const uint8_t* buffer, size_t dimension) {
                return *reinterpret_cast<const float*>(buffer + dimension * sizeof(int8_t)
                                                       + get_sign_storage_size(dimension));
            }

            // Storage layout:
            // [int8 payload | residual-sign bitset | scale]
            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer(const std::vector<float>& input) {
                if(input.empty()) {
                    return std::vector<uint8_t>();
                }

                std::vector<float> rotated = input;
                rotate_pairwise_inplace(rotated);

                const size_t dimension = rotated.size();
                std::vector<uint8_t> buffer(get_storage_size(dimension));

                float abs_max = ndd::quant::math::find_abs_max(rotated.data(), dimension);
                if(abs_max == 0.0f) {
                    abs_max = 1.0f;
                }

                const float scale = abs_max / INT8_SCALE;
                const float inv_scale = 1.0f / scale;

                int8_t* data_ptr = reinterpret_cast<int8_t*>(buffer.data());
                uint64_t* sign_words = extract_sign_words(buffer.data(), dimension);
                const size_t sign_word_count = get_sign_word_count(dimension);
                for(size_t w = 0; w < sign_word_count; ++w) {
                    sign_words[w] = 0ULL;
                }

                for(size_t i = 0; i < dimension; ++i) {
                    const float scaled_real = rotated[i] * inv_scale;
                    const int8_t q = static_cast<int8_t>(std::nearbyint(scaled_real));
                    data_ptr[i] = q;

                    // Bit is 1 for non-negative residual, 0 for negative residual.
                    const float residual = scaled_real - static_cast<float>(q);
                    if(residual >= 0.0f) {
                        const size_t word = i >> 6;
                        const size_t bit = i & 63;
                        sign_words[word] |= (1ULL << bit);
                    }
                }

                float* scale_ptr = reinterpret_cast<float*>(buffer.data() + dimension * sizeof(int8_t)
                                                           + get_sign_storage_size(dimension));
                *scale_ptr = scale;
                return buffer;
            }

#if defined(USE_AVX512)
            // AVX512: single-pass quantize + sign bits.
            // Processes 64 elements per outer iteration (one sign word) using 4×16 chunks,
            // computing quantised values and their residual-sign bits in one loop.
            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer_avx512(const std::vector<float>& input) {
                if(input.empty()) {
                    return std::vector<uint8_t>();
                }

                std::vector<float> rotated = input;
                rotate_pairwise_inplace(rotated);

                const size_t dimension = rotated.size();
                std::vector<uint8_t> buffer(get_storage_size(dimension));

                float abs_max = ndd::quant::math::find_abs_max(rotated.data(), dimension);
                if(abs_max == 0.0f) {
                    abs_max = 1.0f;
                }

                const float scale     = abs_max / INT8_SCALE;
                const float inv_scale = 1.0f / scale;

                int8_t*   data_ptr        = reinterpret_cast<int8_t*>(buffer.data());
                uint64_t* sign_words      = extract_sign_words(buffer.data(), dimension);
                const size_t sign_word_count = get_sign_word_count(dimension);
                for(size_t w = 0; w < sign_word_count; ++w) sign_words[w] = 0ULL;

                const __m512 sc_vec   = _mm512_set1_ps(inv_scale);
                const __m512 zero_vec = _mm512_setzero_ps();

                size_t i = 0;
                // Process 64 elements (one sign word) per iteration via 4×16 AVX-512 chunks.
                // Scale, round, pack to int8, and extract residual-sign mask in a single pass.
                for(; i + 64 <= dimension; i += 64) {
                    __m512 s0 = _mm512_mul_ps(_mm512_loadu_ps(&rotated[i     ]), sc_vec);
                    __m512 s1 = _mm512_mul_ps(_mm512_loadu_ps(&rotated[i + 16]), sc_vec);
                    __m512 s2 = _mm512_mul_ps(_mm512_loadu_ps(&rotated[i + 32]), sc_vec);
                    __m512 s3 = _mm512_mul_ps(_mm512_loadu_ps(&rotated[i + 48]), sc_vec);

                    __m512i q0 = _mm512_cvtps_epi32(s0);
                    __m512i q1 = _mm512_cvtps_epi32(s1);
                    __m512i q2 = _mm512_cvtps_epi32(s2);
                    __m512i q3 = _mm512_cvtps_epi32(s3);

                    _mm_storeu_si128((__m128i*)(data_ptr + i     ), _mm512_cvtepi32_epi8(q0));
                    _mm_storeu_si128((__m128i*)(data_ptr + i + 16), _mm512_cvtepi32_epi8(q1));
                    _mm_storeu_si128((__m128i*)(data_ptr + i + 32), _mm512_cvtepi32_epi8(q2));
                    _mm_storeu_si128((__m128i*)(data_ptr + i + 48), _mm512_cvtepi32_epi8(q3));

                        // Residual = scaled − round(scaled); sign bit = (residual >= 0)
                        // _mm512_cmp_ps_mask returns a 16-bit lane mask; pack four into one uint64.
                    sign_words[i >> 6] =
                            (uint64_t)(uint16_t)_mm512_cmp_ps_mask(
                                _mm512_sub_ps(s0, _mm512_cvtepi32_ps(q0)),
                                zero_vec,
                                _CMP_GE_OQ)
                            | ((uint64_t)(uint16_t)_mm512_cmp_ps_mask(
                                   _mm512_sub_ps(s1, _mm512_cvtepi32_ps(q1)),
                                   zero_vec,
                                   _CMP_GE_OQ)
                               << 16)
                            | ((uint64_t)(uint16_t)_mm512_cmp_ps_mask(
                                   _mm512_sub_ps(s2, _mm512_cvtepi32_ps(q2)),
                                   zero_vec,
                                   _CMP_GE_OQ)
                               << 32)
                            | ((uint64_t)(uint16_t)_mm512_cmp_ps_mask(
                                   _mm512_sub_ps(s3, _mm512_cvtepi32_ps(q3)),
                                   zero_vec,
                                   _CMP_GE_OQ)
                               << 48);
                }

                // Scalar tail (fewer than 64 remaining elements)
                for(; i < dimension; ++i) {
                    const float scaled_real = rotated[i] * inv_scale;
                    const int8_t q          = static_cast<int8_t>(std::nearbyint(scaled_real));
                    data_ptr[i]             = q;
                    if(scaled_real - static_cast<float>(q) >= 0.0f) {
                        sign_words[i >> 6] |= (1ULL << (i & 63));
                    }
                }

                float* scale_ptr = reinterpret_cast<float*>(buffer.data() + dimension * sizeof(int8_t)
                                                             + get_sign_storage_size(dimension));
                *scale_ptr = scale;
                return buffer;
            }
#endif

#if defined(USE_AVX2)
            // AVX2: single-pass quantize + sign bits.
            // Processes 32 elements per iteration (4×8) building 32 sign bits via movemask.
            // Each 32-element block maps directly to the lower or upper half of a sign word.
            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer_avx2(const std::vector<float>& input) {
                if(input.empty()) {
                    return std::vector<uint8_t>();
                }

                std::vector<float> rotated = input;
                rotate_pairwise_inplace(rotated);

                const size_t dimension = rotated.size();
                std::vector<uint8_t> buffer(get_storage_size(dimension));

                float abs_max = ndd::quant::math::find_abs_max(rotated.data(), dimension);
                if(abs_max == 0.0f) {
                    abs_max = 1.0f;
                }

                const float scale     = abs_max / INT8_SCALE;
                const float inv_scale = 1.0f / scale;

                int8_t*   data_ptr        = reinterpret_cast<int8_t*>(buffer.data());
                uint64_t* sign_words      = extract_sign_words(buffer.data(), dimension);
                const size_t sign_word_count = get_sign_word_count(dimension);
                for(size_t w = 0; w < sign_word_count; ++w) sign_words[w] = 0ULL;

                const __m256 sc_vec   = _mm256_set1_ps(inv_scale);
                const __m256 zero_vec = _mm256_setzero_ps();

                size_t i = 0;
                // 32 elements per iteration; i is always a multiple of 32, so bit_off ∈ {0, 32}.
                for(; i + 32 <= dimension; i += 32) {
                    __m256 s0 = _mm256_mul_ps(_mm256_loadu_ps(&rotated[i     ]), sc_vec);
                    __m256 s1 = _mm256_mul_ps(_mm256_loadu_ps(&rotated[i +  8]), sc_vec);
                    __m256 s2 = _mm256_mul_ps(_mm256_loadu_ps(&rotated[i + 16]), sc_vec);
                    __m256 s3 = _mm256_mul_ps(_mm256_loadu_ps(&rotated[i + 24]), sc_vec);

                    __m256i q0 = _mm256_cvtps_epi32(s0);
                    __m256i q1 = _mm256_cvtps_epi32(s1);
                    __m256i q2 = _mm256_cvtps_epi32(s2);
                    __m256i q3 = _mm256_cvtps_epi32(s3);

                    // Pack int32×8 → int16×16 → int8×32
                    __m256i p01 = _mm256_packs_epi32(q0, q1);
                    __m256i p23 = _mm256_packs_epi32(q2, q3);
                    p01         = _mm256_permute4x64_epi64(p01, _MM_SHUFFLE(3, 1, 2, 0));
                    p23         = _mm256_permute4x64_epi64(p23, _MM_SHUFFLE(3, 1, 2, 0));
                    __m256i p   = _mm256_packs_epi16(p01, p23);
                    p           = _mm256_permute4x64_epi64(p, _MM_SHUFFLE(3, 1, 2, 0));
                    _mm256_storeu_si256((__m256i*)(data_ptr + i), p);

                    // _mm256_movemask_ps reads the MSB of each float lane.
                    // The comparison result is 0xFFFFFFFF (MSB=1) or 0 (MSB=0), so this
                    // directly yields one sign bit per element as an 8-bit mask.
                    uint32_t bits32 =
                            (uint32_t)(uint8_t)_mm256_movemask_ps(_mm256_cmp_ps(
                                    _mm256_sub_ps(s0, _mm256_cvtepi32_ps(q0)), zero_vec, _CMP_GE_OQ))
                            | ((uint32_t)(uint8_t)_mm256_movemask_ps(_mm256_cmp_ps(
                                       _mm256_sub_ps(s1, _mm256_cvtepi32_ps(q1)), zero_vec, _CMP_GE_OQ))
                               << 8)
                            | ((uint32_t)(uint8_t)_mm256_movemask_ps(_mm256_cmp_ps(
                                       _mm256_sub_ps(s2, _mm256_cvtepi32_ps(q2)), zero_vec, _CMP_GE_OQ))
                               << 16)
                            | ((uint32_t)(uint8_t)_mm256_movemask_ps(_mm256_cmp_ps(
                                       _mm256_sub_ps(s3, _mm256_cvtepi32_ps(q3)), zero_vec, _CMP_GE_OQ))
                               << 24);
                    // i & 63 == 0 → lower half of sign word; i & 63 == 32 → upper half.
                    sign_words[i >> 6] |= ((uint64_t)bits32 << (i & 63));
                }

                // Scalar tail
                for(; i < dimension; ++i) {
                    const float scaled_real = rotated[i] * inv_scale;
                    const int8_t q          = static_cast<int8_t>(std::nearbyint(scaled_real));
                    data_ptr[i]             = q;
                    if(scaled_real - static_cast<float>(q) >= 0.0f) {
                        sign_words[i >> 6] |= (1ULL << (i & 63));
                    }
                }

                float* scale_ptr = reinterpret_cast<float*>(buffer.data() + dimension * sizeof(int8_t)
                                                             + get_sign_storage_size(dimension));
                *scale_ptr = scale;
                return buffer;
            }
#endif

#if defined(USE_NEON)
            // NEON: single-pass quantize + sign bits.
            // Processes 16 elements per iteration; extracts 16 sign bits via narrow-then-MSB trick.
            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer_neon(const std::vector<float>& input) {
                if(input.empty()) {
                    return std::vector<uint8_t>();
                }

                std::vector<float> rotated = input;
                rotate_pairwise_inplace(rotated);

                const size_t dimension = rotated.size();
                std::vector<uint8_t> buffer(get_storage_size(dimension));

                float abs_max = ndd::quant::math::find_abs_max(rotated.data(), dimension);
                if(abs_max == 0.0f) {
                    abs_max = 1.0f;
                }

                const float scale     = abs_max / INT8_SCALE;
                const float inv_scale = 1.0f / scale;

                int8_t*   data_ptr        = reinterpret_cast<int8_t*>(buffer.data());
                uint64_t* sign_words      = extract_sign_words(buffer.data(), dimension);
                const size_t sign_word_count = get_sign_word_count(dimension);
                for(size_t w = 0; w < sign_word_count; ++w) sign_words[w] = 0ULL;

                const float32x4_t sc_vec  = vdupq_n_f32(inv_scale);
                const float32x4_t zero4   = vdupq_n_f32(0.0f);
                // Powers-of-two for bit-packing: byte_k = Σ (bit_j * 2^j)
                static const uint8_t powers_arr[] = {1, 2, 4, 8, 16, 32, 64, 128};
                const uint8x8_t v_powers = vld1_u8(powers_arr);

                size_t i = 0;
                for(; i + 16 <= dimension; i += 16) {
                    float32x4_t s0 = vmulq_f32(vld1q_f32(&rotated[i     ]), sc_vec);
                    float32x4_t s1 = vmulq_f32(vld1q_f32(&rotated[i +  4]), sc_vec);
                    float32x4_t s2 = vmulq_f32(vld1q_f32(&rotated[i +  8]), sc_vec);
                    float32x4_t s3 = vmulq_f32(vld1q_f32(&rotated[i + 12]), sc_vec);

                    int32x4_t q0 = vcvtnq_s32_f32(s0);  // round to nearest, ties to even (C1)
                    int32x4_t q1 = vcvtnq_s32_f32(s1);
                    int32x4_t q2 = vcvtnq_s32_f32(s2);
                    int32x4_t q3 = vcvtnq_s32_f32(s3);

                    // Pack to int8
                    int16x8_t p01 = vcombine_s16(vqmovn_s32(q0), vqmovn_s32(q1));
                    int16x8_t p23 = vcombine_s16(vqmovn_s32(q2), vqmovn_s32(q3));
                    vst1q_s8(&data_ptr[i], vcombine_s8(vqmovn_s16(p01), vqmovn_s16(p23)));

                    // Residual comparison: 0xFFFFFFFF if (s - round(s)) >= 0
                    uint32x4_t m0 = vcgeq_f32(vsubq_f32(s0, vcvtq_f32_s32(q0)), zero4);
                    uint32x4_t m1 = vcgeq_f32(vsubq_f32(s1, vcvtq_f32_s32(q1)), zero4);
                    uint32x4_t m2 = vcgeq_f32(vsubq_f32(s2, vcvtq_f32_s32(q2)), zero4);
                    uint32x4_t m3 = vcgeq_f32(vsubq_f32(s3, vcvtq_f32_s32(q3)), zero4);

                    // Narrow chain: 32-bit (0/0xFFFFFFFF) → 16-bit → 8-bit → 1-bit via MSB
                    uint8x8_t b01 = vshrn_n_u16(
                            vcombine_u16(vshrn_n_u32(m0, 16), vshrn_n_u32(m1, 16)), 8);
                    uint8x8_t b23 = vshrn_n_u16(
                            vcombine_u16(vshrn_n_u32(m2, 16), vshrn_n_u32(m3, 16)), 8);
                    // MSB of each byte: 0xFF → 0x01, 0x00 → 0x00; dot with powers of 2
                    uint8_t byte0 = vaddv_u8(vmul_u8(vshr_n_u8(b01, 7), v_powers));
                    uint8_t byte1 = vaddv_u8(vmul_u8(vshr_n_u8(b23, 7), v_powers));

                    // i is always a multiple of 16; i & 63 ∈ {0, 16, 32, 48}
                    sign_words[i >> 6] |=
                            ((uint64_t)byte0 | ((uint64_t)byte1 << 8)) << (i & 63);
                }

                // Scalar tail
                for(; i < dimension; ++i) {
                    const float scaled_real = rotated[i] * inv_scale;
                    const int8_t q          = static_cast<int8_t>(std::nearbyint(scaled_real));
                    data_ptr[i]             = q;
                    if(scaled_real - static_cast<float>(q) >= 0.0f) {
                        sign_words[i >> 6] |= (1ULL << (i & 63));
                    }
                }

                float* scale_ptr = reinterpret_cast<float*>(buffer.data() + dimension * sizeof(int8_t)
                                                             + get_sign_storage_size(dimension));
                *scale_ptr = scale;
                return buffer;
            }
#endif

#if defined(USE_SVE2)
            // SVE2: vectorized quantize with scalar sign-bit extraction.
            // Quantisation uses full-width SVE predicated loads/stores.
            // Residual sign packing is scalar to guarantee correctness across all SVE widths.
            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer_sve(const std::vector<float>& input) {
                if(input.empty()) {
                    return std::vector<uint8_t>();
                }

                std::vector<float> rotated = input;
                rotate_pairwise_inplace(rotated);

                const size_t dimension = rotated.size();
                std::vector<uint8_t> buffer(get_storage_size(dimension));

                float abs_max = ndd::quant::math::find_abs_max(rotated.data(), dimension);
                if(abs_max == 0.0f) {
                    abs_max = 1.0f;
                }

                const float scale     = abs_max / INT8_SCALE;
                const float inv_scale = 1.0f / scale;

                int8_t*   data_ptr        = reinterpret_cast<int8_t*>(buffer.data());
                uint64_t* sign_words      = extract_sign_words(buffer.data(), dimension);
                const size_t sign_word_count = get_sign_word_count(dimension);
                for(size_t w = 0; w < sign_word_count; ++w) sign_words[w] = 0ULL;

                // Pass 1: vectorized quantization on full SVE-width chunks.
                // Remainder elements are handled in scalar to avoid partial-lane SIMD tails.
                {
                    size_t i = 0;
                    const size_t vecw = svcntw();
                    for(; i + vecw <= dimension; i += vecw) {
                        svfloat32_t v = svmul_n_f32_x(svptrue_b32(),
                                                      svld1_f32(svptrue_b32(), &rotated[i]),
                                                      inv_scale);
                        v = svrintn_f32_x(svptrue_b32(), v);  // round to nearest, ties to even (C1)
                        svint32_t q = svcvt_s32_f32_x(svptrue_b32(), v);
                        q = svmin_s32_x(svptrue_b32(), q, svdup_s32(127));
                        q = svmax_s32_x(svptrue_b32(), q, svdup_s32(-127));
                        svst1b_s32(svptrue_b32(), &data_ptr[i], q);
                    }

                    for(; i < dimension; ++i) {
                        const float scaled_real = rotated[i] * inv_scale;
                        data_ptr[i] = static_cast<int8_t>(std::nearbyint(scaled_real));
                    }
                }

                // Pass 2: residual-sign bit extraction.
                // Use scalar bit packing for correctness across all possible SVE widths.
                for(size_t w = 0; w < sign_word_count; ++w) {
                    const size_t base  = w * 64;
                    uint64_t sign_word = 0;

                    const size_t end = std::min(base + 64, dimension);
                    for(size_t idx = base; idx < end; ++idx) {
                        const float scaled_real = rotated[idx] * inv_scale;
                        const float residual = scaled_real - static_cast<float>(data_ptr[idx]);
                        if(residual >= 0.0f) {
                            sign_word |= (1ULL << (idx - base));
                        }
                    }

                    sign_words[w] = sign_word;
                }

                float* scale_ptr = reinterpret_cast<float*>(buffer.data() + dimension * sizeof(int8_t)
                                                             + get_sign_storage_size(dimension));
                *scale_ptr = scale;
                return buffer;
            }
#endif

            inline std::vector<uint8_t>
            quantize_vector_fp32_to_int8e_buffer_auto(const std::vector<float>& input) {
#if defined(USE_AVX512)
                return quantize_vector_fp32_to_int8e_buffer_avx512(input);
#elif defined(USE_AVX2)
                return quantize_vector_fp32_to_int8e_buffer_avx2(input);
#elif defined(USE_SVE2)
                return quantize_vector_fp32_to_int8e_buffer_sve(input);
#elif defined(USE_NEON)
                return quantize_vector_fp32_to_int8e_buffer_neon(input);
#else
                return quantize_vector_fp32_to_int8e_buffer(input);
#endif
            }

            inline std::vector<float> dequantize_int8e_buffer_to_fp32_scalar(const uint8_t* buffer,
                                                                             size_t dimension) {
                std::vector<float> out(dimension);

                const int8_t* payload = reinterpret_cast<const int8_t*>(buffer);
                const uint64_t* sign_words = extract_sign_words(buffer, dimension);
                const float scale = extract_scale(buffer, dimension);

                for(size_t i = 0; i < dimension; ++i) {
                    const size_t word = i >> 6;
                    const size_t bit = i & 63;
                    const float residual_center = ((sign_words[word] >> bit) & 1ULL) != 0ULL
                                                          ? 0.25f
                                                          : -0.25f;
                    out[i] = (static_cast<float>(payload[i]) + residual_center) * scale;
                }

                rotate_pairwise_inplace(out);
                return out;
            }

#if defined(USE_AVX512)
            // AVX512: 4×16 unrolled dequantize + vectorised sign correction.
            // Sign correction uses _mm512_mask_blend_ps: the 16-bit sign-word chunk becomes
            // a __mmask16 that selects +corr or -corr for each lane in one instruction.
            inline std::vector<float> dequantize_int8e_buffer_to_fp32_avx512(const uint8_t* buffer,
                                                                             size_t dimension) {
                std::vector<float> out(dimension);
                const int8_t*    payload    = reinterpret_cast<const int8_t*>(buffer);
                const uint64_t*  sign_words = extract_sign_words(buffer, dimension);
                const float      scale      = extract_scale(buffer, dimension);
                const __m512     scale_vec  = _mm512_set1_ps(scale);

                // Fused single pass: dequant + residual-sign correction in one store.
                // Computes (payload + center)*scale (one rounding) -> bit-identical to scalar.
                const __m512 pos = _mm512_set1_ps(0.25f);
                const __m512 neg = _mm512_set1_ps(-0.25f);
                size_t i = 0;
                for(; i + 16 <= dimension; i += 16) {
                    const __mmask16 m = static_cast<__mmask16>(
                            static_cast<uint16_t>(sign_words[i >> 6] >> (i & 63)));
                    __m512 center = _mm512_mask_blend_ps(m, neg, pos);
                    __m512 pf = _mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(
                            _mm_loadu_si128((const __m128i*)(payload + i))));
                    _mm512_storeu_ps(&out[i],
                                     _mm512_mul_ps(_mm512_add_ps(pf, center), scale_vec));
                }
                for(; i < dimension; ++i) {
                    const float c = ((sign_words[i >> 6] >> (i & 63)) & 1ULL) ? 0.25f : -0.25f;
                    out[i] = (static_cast<float>(payload[i]) + c) * scale;
                }

                rotate_pairwise_inplace(out);
                return out;
            }
#endif

#if defined(USE_AVX2)
            inline std::vector<float> dequantize_int8e_buffer_to_fp32_avx2(const uint8_t* buffer,
                                                                           size_t dimension) {
                std::vector<float> out(dimension);
                const int8_t* payload = reinterpret_cast<const int8_t*>(buffer);
                const uint64_t* sign_words = extract_sign_words(buffer, dimension);
                const float scale = extract_scale(buffer, dimension);
                const __m256 scale_vec = _mm256_set1_ps(scale);

                // Fused single pass: dequant + residual-sign correction in one store.
                // Computes (payload + center)*scale (one rounding) -> bit-identical to scalar.
                const __m256 pos = _mm256_set1_ps(0.25f);
                const __m256 neg = _mm256_set1_ps(-0.25f);
                const __m256i shift_idx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
                const __m256i one_vec   = _mm256_set1_epi32(1);
                size_t i = 0;
                for(; i + 8 <= dimension; i += 8) {
                    const uint8_t m8 = static_cast<uint8_t>(sign_words[i >> 6] >> (i & 63));
                    __m256i m = _mm256_srlv_epi32(_mm256_set1_epi32((int)m8), shift_idx);
                    m         = _mm256_cmpeq_epi32(_mm256_and_si256(m, one_vec), one_vec);
                    __m256 center = _mm256_blendv_ps(neg, pos, _mm256_castsi256_ps(m));
                    __m256 pf = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(
                            _mm_loadl_epi64((const __m128i*)(payload + i))));
                    _mm256_storeu_ps(&out[i],
                                     _mm256_mul_ps(_mm256_add_ps(pf, center), scale_vec));
                }
                for(; i < dimension; ++i) {
                    const float c = ((sign_words[i >> 6] >> (i & 63)) & 1ULL) ? 0.25f : -0.25f;
                    out[i] = (static_cast<float>(payload[i]) + c) * scale;
                }

                rotate_pairwise_inplace(out);
                return out;
            }
#endif

#if defined(USE_NEON)
            // NEON dequantize + vectorised sign correction.
            // Sign correction processes 4 elements per NEON op using vbslq_f32 with a mask
            // built from (uint32_t)-((nibble >> k) & 1): 0 → 0, 1 → 0xFFFFFFFF.
            inline std::vector<float> dequantize_int8e_buffer_to_fp32_neon(const uint8_t* buffer,
                                                                           size_t dimension) {
                std::vector<float> out(dimension);
                const int8_t*    payload    = reinterpret_cast<const int8_t*>(buffer);
                const uint64_t*  sign_words = extract_sign_words(buffer, dimension);
                const float      scale      = extract_scale(buffer, dimension);
                const float32x4_t scale_vec = vdupq_n_f32(scale);

                // Fused single pass: dequant + residual-sign correction in one store.
                // Computes (payload + center)*scale (one rounding) -> bit-identical to scalar.
                const float32x4_t pos = vdupq_n_f32(0.25f), neg = vdupq_n_f32(-0.25f);
                size_t i = 0;
                for(; i + 4 <= dimension; i += 4) {
                    const uint8_t nib = static_cast<uint8_t>(sign_words[i >> 6] >> (i & 63)) & 0xFu;
                    const uint32x4_t mv = {
                        (uint32_t)-((nib >> 0) & 1u), (uint32_t)-((nib >> 1) & 1u),
                        (uint32_t)-((nib >> 2) & 1u), (uint32_t)-((nib >> 3) & 1u) };
                    float32x4_t center = vbslq_f32(mv, pos, neg);
                    int32x4_t pi = vmovl_s16(vget_low_s16(vmovl_s8(vld1_s8(payload + i))));
                    vst1q_f32(&out[i], vmulq_f32(vaddq_f32(vcvtq_f32_s32(pi), center), scale_vec));
                }
                for(; i < dimension; ++i) {
                    const float c = ((sign_words[i >> 6] >> (i & 63)) & 1ULL) ? 0.25f : -0.25f;
                    out[i] = (static_cast<float>(payload[i]) + c) * scale;
                }

                rotate_pairwise_inplace(out);
                return out;
            }
#endif

#if defined(USE_SVE2)
            // SVE2 dequantize with scalar sign correction.
            // Dequantization remains vectorized; sign correction is scalar for correctness
            // across all SVE vector lengths.
            inline std::vector<float> dequantize_int8e_buffer_to_fp32_sve(const uint8_t* buffer,
                                                                          size_t dimension) {
                std::vector<float> out(dimension);
                const int8_t*   payload    = reinterpret_cast<const int8_t*>(buffer);
                const uint64_t* sign_words = extract_sign_words(buffer, dimension);
                const float     scale      = extract_scale(buffer, dimension);

                // Fused single pass: dequant + residual-sign correction (per-lane predicate
                // built from the sign bits). Computes (payload + center)*scale (one rounding)
                // -> bit-identical to scalar. VL-agnostic (svcntw chunks within one sign word).
                const size_t vw = svcntw();
                for(size_t i = 0; i < dimension; i += vw) {
                    svbool_t pg = svwhilelt_b32((uint64_t)i, (uint64_t)dimension);
                    const uint32_t cbits = static_cast<uint32_t>(sign_words[i >> 6] >> (i & 63));
                    svuint32_t lane = svindex_u32(0, 1);
                    svbool_t sel = svcmpne_n_u32(pg,
                            svand_n_u32_x(pg, svlsr_u32_x(pg, svdup_u32(cbits), lane), 1), 0);
                    svint32_t pi = svld1sb_s32(pg, payload + i);
                    svfloat32_t pf = svcvt_f32_s32_x(pg, pi);
                    svfloat32_t center = svsel_f32(sel, svdup_f32(0.25f), svdup_f32(-0.25f));
                    svst1_f32(pg, &out[i], svmul_n_f32_x(pg, svadd_f32_x(pg, pf, center), scale));
                }

                rotate_pairwise_inplace(out);
                return out;
            }
#endif

            inline std::vector<float> dequantize_int8e_buffer_to_fp32(const uint8_t* buffer,
                                                                      size_t dimension) {
#if defined(USE_AVX512)
                return dequantize_int8e_buffer_to_fp32_avx512(buffer, dimension);
#elif defined(USE_AVX2)
                return dequantize_int8e_buffer_to_fp32_avx2(buffer, dimension);
#elif defined(USE_SVE2)
                return dequantize_int8e_buffer_to_fp32_sve(buffer, dimension);
#elif defined(USE_NEON)
                return dequantize_int8e_buffer_to_fp32_neon(buffer, dimension);
#else
                return dequantize_int8e_buffer_to_fp32_scalar(buffer, dimension);
#endif
            }

            inline std::vector<uint8_t> quantize(const std::vector<float>& input) {
                return quantize_vector_fp32_to_int8e_buffer_auto(input);
            }

            inline std::vector<float> dequantize(const uint8_t* in, size_t dim) {
                return dequantize_int8e_buffer_to_fp32(in, dim);
            }

            static float L2Sqr(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                const int8_t* pVect1 = static_cast<const int8_t*>(pVect1v);
                const int8_t* pVect2 = static_cast<const int8_t*>(pVect2v);
                const auto* params = static_cast<const hnswlib::DistParams*>(qty_ptr);
                const size_t qty = params->dim;

                const float scale1 = extract_scale(reinterpret_cast<const uint8_t*>(pVect1), qty);
                const float scale2 = extract_scale(reinterpret_cast<const uint8_t*>(pVect2), qty);
                const uint64_t* bits1 = extract_sign_words(reinterpret_cast<const uint8_t*>(pVect1),
                                                           qty);
                const uint64_t* bits2 = extract_sign_words(reinterpret_cast<const uint8_t*>(pVect2),
                                                           qty);

                int64_t sum_ab = 0;
                int64_t sum_a2 = 0;
                int64_t sum_b2 = 0;
                int64_t sum_a = 0;
                int64_t sum_b = 0;
                size_t i = 0;

#if defined(USE_AVX512)
                {
                    __m512i ab_acc = _mm512_setzero_si512();
                    __m512i a2_acc = _mm512_setzero_si512();
                    __m512i b2_acc = _mm512_setzero_si512();
                    __m512i sa_acc = _mm512_setzero_si512();
                    __m512i sb_acc = _mm512_setzero_si512();
                    const __m512i ones16 = _mm512_set1_epi16(1);

                    for(; i + 32 <= qty; i += 32) {
                        __m256i a8 = _mm256_loadu_si256((const __m256i*)(pVect1 + i));
                        __m256i b8 = _mm256_loadu_si256((const __m256i*)(pVect2 + i));

                        __m512i a16 = _mm512_cvtepi8_epi16(a8);
                        __m512i b16 = _mm512_cvtepi8_epi16(b8);

                        ab_acc = _mm512_add_epi32(ab_acc, _mm512_madd_epi16(a16, b16));
                        a2_acc = _mm512_add_epi32(a2_acc, _mm512_madd_epi16(a16, a16));
                        b2_acc = _mm512_add_epi32(b2_acc, _mm512_madd_epi16(b16, b16));
                        sa_acc = _mm512_add_epi32(sa_acc, _mm512_madd_epi16(a16, ones16));
                        sb_acc = _mm512_add_epi32(sb_acc, _mm512_madd_epi16(b16, ones16));
                    }

                    sum_ab += static_cast<int64_t>(_mm512_reduce_add_epi32(ab_acc));
                    sum_a2 += static_cast<int64_t>(_mm512_reduce_add_epi32(a2_acc));
                    sum_b2 += static_cast<int64_t>(_mm512_reduce_add_epi32(b2_acc));
                    sum_a += static_cast<int64_t>(_mm512_reduce_add_epi32(sa_acc));
                    sum_b += static_cast<int64_t>(_mm512_reduce_add_epi32(sb_acc));
                }
#elif defined(USE_AVX2)
                {
                    __m256i ab_acc = _mm256_setzero_si256();
                    __m256i a2_acc = _mm256_setzero_si256();
                    __m256i b2_acc = _mm256_setzero_si256();
                    __m256i sa_acc = _mm256_setzero_si256();
                    __m256i sb_acc = _mm256_setzero_si256();
                    const __m256i ones16 = _mm256_set1_epi16(1);

                    for(; i + 16 <= qty; i += 16) {
                        __m128i a8 = _mm_loadu_si128((const __m128i*)(pVect1 + i));
                        __m128i b8 = _mm_loadu_si128((const __m128i*)(pVect2 + i));

                        __m256i a16 = _mm256_cvtepi8_epi16(a8);
                        __m256i b16 = _mm256_cvtepi8_epi16(b8);

                        ab_acc = _mm256_add_epi32(ab_acc, _mm256_madd_epi16(a16, b16));
                        a2_acc = _mm256_add_epi32(a2_acc, _mm256_madd_epi16(a16, a16));
                        b2_acc = _mm256_add_epi32(b2_acc, _mm256_madd_epi16(b16, b16));
                        sa_acc = _mm256_add_epi32(sa_acc, _mm256_madd_epi16(a16, ones16));
                        sb_acc = _mm256_add_epi32(sb_acc, _mm256_madd_epi16(b16, ones16));
                    }

                    auto reduce_epi32_avx2 = [](__m256i v) -> int64_t {
                        __m128i lo = _mm256_castsi256_si128(v);
                        __m128i hi = _mm256_extracti128_si256(v, 1);
                        __m128i s = _mm_add_epi32(lo, hi);
                        s = _mm_hadd_epi32(s, s);
                        s = _mm_hadd_epi32(s, s);
                        return static_cast<int64_t>(_mm_cvtsi128_si32(s));
                    };

                    sum_ab += reduce_epi32_avx2(ab_acc);
                    sum_a2 += reduce_epi32_avx2(a2_acc);
                    sum_b2 += reduce_epi32_avx2(b2_acc);
                    sum_a += reduce_epi32_avx2(sa_acc);
                    sum_b += reduce_epi32_avx2(sb_acc);
                }
#elif defined(USE_NEON) && defined(__ARM_FEATURE_DOTPROD)
                {
                    int32x4_t ab_acc = vdupq_n_s32(0);
                    int32x4_t a2_acc = vdupq_n_s32(0);
                    int32x4_t b2_acc = vdupq_n_s32(0);
                    int32x4_t sa_acc = vdupq_n_s32(0);
                    int32x4_t sb_acc = vdupq_n_s32(0);
                    const int8x16_t ones8 = vdupq_n_s8(1);

                    for(; i + 16 <= qty; i += 16) {
                        int8x16_t a8 = vld1q_s8(pVect1 + i);
                        int8x16_t b8 = vld1q_s8(pVect2 + i);

                        ab_acc = vdotq_s32(ab_acc, a8, b8);
                        a2_acc = vdotq_s32(a2_acc, a8, a8);
                        b2_acc = vdotq_s32(b2_acc, b8, b8);
                        sa_acc = vdotq_s32(sa_acc, a8, ones8);
                        sb_acc = vdotq_s32(sb_acc, b8, ones8);
                    }

                    sum_ab += static_cast<int64_t>(vaddvq_s32(ab_acc));
                    sum_a2 += static_cast<int64_t>(vaddvq_s32(a2_acc));
                    sum_b2 += static_cast<int64_t>(vaddvq_s32(b2_acc));
                    sum_a += static_cast<int64_t>(vaddvq_s32(sa_acc));
                    sum_b += static_cast<int64_t>(vaddvq_s32(sb_acc));
                }
#elif defined(USE_SVE2)
                {
                    svint32_t ab_acc = svdup_s32(0);
                    svint32_t a2_acc = svdup_s32(0);
                    svint32_t b2_acc = svdup_s32(0);
                    svint32_t sa_acc = svdup_s32(0);
                    svint32_t sb_acc = svdup_s32(0);
                    const size_t vec_bytes = svcntb();
                    const svint8_t ones8 = svdup_s8(1);

                    for(; i + vec_bytes <= qty; i += vec_bytes) {
                        svint8_t a8 = svld1_s8(svptrue_b8(), pVect1 + i);
                        svint8_t b8 = svld1_s8(svptrue_b8(), pVect2 + i);

                        ab_acc = svdot_s32(ab_acc, a8, b8);
                        a2_acc = svdot_s32(a2_acc, a8, a8);
                        b2_acc = svdot_s32(b2_acc, b8, b8);
                        sa_acc = svdot_s32(sa_acc, a8, ones8);
                        sb_acc = svdot_s32(sb_acc, b8, ones8);
                    }

                    sum_ab += static_cast<int64_t>(svaddv_s32(svptrue_b32(), ab_acc));
                    sum_a2 += static_cast<int64_t>(svaddv_s32(svptrue_b32(), a2_acc));
                    sum_b2 += static_cast<int64_t>(svaddv_s32(svptrue_b32(), b2_acc));
                    sum_a += static_cast<int64_t>(svaddv_s32(svptrue_b32(), sa_acc));
                    sum_b += static_cast<int64_t>(svaddv_s32(svptrue_b32(), sb_acc));
                }
#endif

                for(; i < qty; ++i) {
                    const int32_t a = static_cast<int32_t>(pVect1[i]);
                    const int32_t b = static_cast<int32_t>(pVect2[i]);
                    sum_ab += static_cast<int64_t>(a) * b;
                    sum_a2 += static_cast<int64_t>(a) * a;
                    sum_b2 += static_cast<int64_t>(b) * b;
                    sum_a += static_cast<int64_t>(a);
                    sum_b += static_cast<int64_t>(b);
                }

                int64_t sum_pos_ai_yi = 0;
                int64_t sum_pos_bi_xi = 0;
                int64_t sum_pos_ai_xi = 0;
                int64_t sum_pos_bi_yi = 0;
                int64_t sum_ai_bi = 0;

                const size_t word_count = get_sign_word_count(qty);
                // Residual-sign correction: each sum_pos_* is a sign-mask-selected sum of the
                // contiguous int8 payload. Vectorised per ISA (scalar gather kept as fallback).
#if defined(USE_AVX512)
                {
                    const __m512i ones_u8 = _mm512_set1_epi8(1);
                    __m512i accAY = _mm512_setzero_si512(), accAX = _mm512_setzero_si512();
                    __m512i accBX = _mm512_setzero_si512(), accBY = _mm512_setzero_si512();
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        // maskz load reads only sign-set lanes (faults suppressed elsewhere)
                        accAY = _mm512_dpbusd_epi32(accAY, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b1, (const void*)(pVect2 + base)));
                        accAX = _mm512_dpbusd_epi32(accAX, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b1, (const void*)(pVect1 + base)));
                        accBX = _mm512_dpbusd_epi32(accBX, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b2, (const void*)(pVect1 + base)));
                        accBY = _mm512_dpbusd_epi32(accBY, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b2, (const void*)(pVect2 + base)));
                    }
                    sum_pos_ai_yi = _mm512_reduce_add_epi32(accAY);
                    sum_pos_ai_xi = _mm512_reduce_add_epi32(accAX);
                    sum_pos_bi_xi = _mm512_reduce_add_epi32(accBX);
                    sum_pos_bi_yi = _mm512_reduce_add_epi32(accBY);
                }
#elif defined(USE_AVX2)
                {
                    const __m256i ones16 = _mm256_set1_epi16(1);
                    __m256i accAY = _mm256_setzero_si256(), accAX = _mm256_setzero_si256();
                    __m256i accBX = _mm256_setzero_si256(), accBY = _mm256_setzero_si256();
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        size_t j = base;
                        for(; j + 16 <= end; j += 16) {
                            const __m128i MA = int8e_mask16_to_bytes((uint16_t)(b1 >> (j - base)));
                            const __m128i MB = int8e_mask16_to_bytes((uint16_t)(b2 >> (j - base)));
                            const __m128i y = _mm_loadu_si128((const __m128i*)(pVect2 + j));
                            const __m128i x = _mm_loadu_si128((const __m128i*)(pVect1 + j));
                            accAY = _mm256_add_epi32(accAY, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(y, MA)), ones16));
                            accAX = _mm256_add_epi32(accAX, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(x, MA)), ones16));
                            accBX = _mm256_add_epi32(accBX, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(x, MB)), ones16));
                            accBY = _mm256_add_epi32(accBY, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(y, MB)), ones16));
                        }
                        for(; j < end; ++j) {
                            const size_t bit = j - base;
                            if((b1 >> bit) & 1ULL) { sum_pos_ai_yi += pVect2[j]; sum_pos_ai_xi += pVect1[j]; }
                            if((b2 >> bit) & 1ULL) { sum_pos_bi_xi += pVect1[j]; sum_pos_bi_yi += pVect2[j]; }
                        }
                    }
                    sum_pos_ai_yi += int8e_hsum_epi32_avx2(accAY);
                    sum_pos_ai_xi += int8e_hsum_epi32_avx2(accAX);
                    sum_pos_bi_xi += int8e_hsum_epi32_avx2(accBX);
                    sum_pos_bi_yi += int8e_hsum_epi32_avx2(accBY);
                }
#elif defined(USE_NEON) && defined(__ARM_FEATURE_DOTPROD)
                {
                    const int8x16_t ones = vdupq_n_s8(1);
                    int32x4_t accAY = vdupq_n_s32(0), accAX = vdupq_n_s32(0), accBX = vdupq_n_s32(0), accBY = vdupq_n_s32(0);
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        size_t j = base;
                        for(; j + 16 <= end; j += 16) {
                            const int8x16_t MA = int8e_mask16_to_bytes_neon((uint16_t)(b1 >> (j - base)));
                            const int8x16_t MB = int8e_mask16_to_bytes_neon((uint16_t)(b2 >> (j - base)));
                            const int8x16_t y = vld1q_s8(pVect2 + j);
                            const int8x16_t x = vld1q_s8(pVect1 + j);
                            accAY = vdotq_s32(accAY, vandq_s8(y, MA), ones);
                            accAX = vdotq_s32(accAX, vandq_s8(x, MA), ones);
                            accBX = vdotq_s32(accBX, vandq_s8(x, MB), ones);
                            accBY = vdotq_s32(accBY, vandq_s8(y, MB), ones);
                        }
                        for(; j < end; ++j) {
                            const size_t bit = j - base;
                            if((b1 >> bit) & 1ULL) { sum_pos_ai_yi += pVect2[j]; sum_pos_ai_xi += pVect1[j]; }
                            if((b2 >> bit) & 1ULL) { sum_pos_bi_xi += pVect1[j]; sum_pos_bi_yi += pVect2[j]; }
                        }
                    }
                    sum_pos_ai_yi += vaddvq_s32(accAY);
                    sum_pos_ai_xi += vaddvq_s32(accAX);
                    sum_pos_bi_xi += vaddvq_s32(accBX);
                    sum_pos_bi_yi += vaddvq_s32(accBY);
                }
#elif defined(USE_SVE2)
                {
                    const svint8_t ones = svdup_s8(1);
                    const size_t vb = svcntb();
                    svint32_t accAY = svdup_s32(0), accAX = svdup_s32(0), accBX = svdup_s32(0), accBY = svdup_s32(0);
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        for(size_t o = 0; base + o < end; o += vb) {
                            svbool_t pg = svwhilelt_b8((uint64_t)(base + o), (uint64_t)end);
                            svbool_t pa = int8e_pred_from_bits(pg, b1 >> o);
                            svbool_t pb = int8e_pred_from_bits(pg, b2 >> o);
                            accAY = svdot_s32(accAY, svld1_s8(pa, pVect2 + base + o), ones);
                            accAX = svdot_s32(accAX, svld1_s8(pa, pVect1 + base + o), ones);
                            accBX = svdot_s32(accBX, svld1_s8(pb, pVect1 + base + o), ones);
                            accBY = svdot_s32(accBY, svld1_s8(pb, pVect2 + base + o), ones);
                        }
                    }
                    sum_pos_ai_yi += svaddv_s32(svptrue_b32(), accAY);
                    sum_pos_ai_xi += svaddv_s32(svptrue_b32(), accAX);
                    sum_pos_bi_xi += svaddv_s32(svptrue_b32(), accBX);
                    sum_pos_bi_yi += svaddv_s32(svptrue_b32(), accBY);
                }
#else
                for(size_t w = 0; w < word_count; ++w) {
                    const size_t base = w * 64;
                    uint64_t mask = ~0ULL;
                    if(base + 64 > qty) {
                        const size_t remaining = qty - base;
                        mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                    }

                    const uint64_t b1 = bits1[w] & mask;
                    const uint64_t b2 = bits2[w] & mask;

                    const int64_t lane_count =
                            static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                    const int64_t diff = static_cast<int64_t>(__builtin_popcountll(b1 ^ b2));
                    sum_ai_bi += lane_count - (diff << 1);

                    uint64_t active1 = b1;
                    while(active1 != 0ULL) {
                        const size_t bit = static_cast<size_t>(__builtin_ctzll(active1));
                        const size_t idx = base + bit;
                        sum_pos_ai_yi += static_cast<int64_t>(pVect2[idx]);
                        sum_pos_ai_xi += static_cast<int64_t>(pVect1[idx]);
                        active1 &= (active1 - 1ULL);
                    }

                    uint64_t active2 = b2;
                    while(active2 != 0ULL) {
                        const size_t bit = static_cast<size_t>(__builtin_ctzll(active2));
                        const size_t idx = base + bit;
                        sum_pos_bi_xi += static_cast<int64_t>(pVect1[idx]);
                        sum_pos_bi_yi += static_cast<int64_t>(pVect2[idx]);
                        active2 &= (active2 - 1ULL);
                    }
                }
#endif

                const int64_t sum_b_ai = (sum_pos_ai_yi << 1) - sum_b;
                const int64_t sum_a_bi = (sum_pos_bi_xi << 1) - sum_a;
                const int64_t sum_a_ai = (sum_pos_ai_xi << 1) - sum_a;
                const int64_t sum_b_bi = (sum_pos_bi_yi << 1) - sum_b;

                const float s1s1 = scale1 * scale1;
                const float s2s2 = scale2 * scale2;
                const float s1s2 = scale1 * scale2;

                const float base = static_cast<float>(sum_a2) * s1s1
                                   + static_cast<float>(sum_b2) * s2s2
                                   - 2.0f * static_cast<float>(sum_ab) * s1s2;

                const float linear = 0.5f
                                     * (static_cast<float>(sum_a_ai) * s1s1
                                        + static_cast<float>(sum_b_bi) * s2s2
                                        - static_cast<float>(sum_a_bi + sum_b_ai) * s1s2);

                const float quadratic = (static_cast<float>(qty) * 0.0625f) * (s1s1 + s2s2)
                                        - 0.125f * static_cast<float>(sum_ai_bi) * s1s2;

                return base + linear + quadratic;
            }

            static float L2SqrSim(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                return -L2Sqr(pVect1v, pVect2v, qty_ptr);
            }

            static float
            InnerProductSim(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                const int8_t* pVect1 = static_cast<const int8_t*>(pVect1v);
                const int8_t* pVect2 = static_cast<const int8_t*>(pVect2v);
                const auto* params = static_cast<const hnswlib::DistParams*>(qty_ptr);
                const size_t qty = params->dim;

                const float scale1 = extract_scale(reinterpret_cast<const uint8_t*>(pVect1), qty);
                const float scale2 = extract_scale(reinterpret_cast<const uint8_t*>(pVect2), qty);
                const uint64_t* bits1 = extract_sign_words(reinterpret_cast<const uint8_t*>(pVect1),
                                                           qty);
                const uint64_t* bits2 = extract_sign_words(reinterpret_cast<const uint8_t*>(pVect2),
                                                           qty);

                int64_t dot = 0;
                int64_t sum_xi = 0;
                int64_t sum_yi = 0;
                size_t i = 0;
#if defined(USE_AVX512)
                {
                    __m512i dot_acc = _mm512_setzero_si512();
                    __m512i sx_acc = _mm512_setzero_si512();
                    __m512i sy_acc = _mm512_setzero_si512();
                    const __m512i ones16 = _mm512_set1_epi16(1);

                    for(; i + 32 <= qty; i += 32) {
                        __m256i a8 = _mm256_loadu_si256((const __m256i*)(pVect1 + i));
                        __m256i b8 = _mm256_loadu_si256((const __m256i*)(pVect2 + i));

                        __m512i a16 = _mm512_cvtepi8_epi16(a8);
                        __m512i b16 = _mm512_cvtepi8_epi16(b8);

                        dot_acc = _mm512_add_epi32(dot_acc, _mm512_madd_epi16(a16, b16));
                        sx_acc = _mm512_add_epi32(sx_acc, _mm512_madd_epi16(a16, ones16));
                        sy_acc = _mm512_add_epi32(sy_acc, _mm512_madd_epi16(b16, ones16));
                    }

                    dot += static_cast<int64_t>(_mm512_reduce_add_epi32(dot_acc));
                    sum_xi += static_cast<int64_t>(_mm512_reduce_add_epi32(sx_acc));
                    sum_yi += static_cast<int64_t>(_mm512_reduce_add_epi32(sy_acc));
                }
#elif defined(USE_AVX2)
                {
                    __m256i dot_acc = _mm256_setzero_si256();
                    __m256i sx_acc = _mm256_setzero_si256();
                    __m256i sy_acc = _mm256_setzero_si256();
                    const __m256i ones16 = _mm256_set1_epi16(1);

                    for(; i + 16 <= qty; i += 16) {
                        __m128i a8 = _mm_loadu_si128((const __m128i*)(pVect1 + i));
                        __m128i b8 = _mm_loadu_si128((const __m128i*)(pVect2 + i));

                        __m256i a16 = _mm256_cvtepi8_epi16(a8);
                        __m256i b16 = _mm256_cvtepi8_epi16(b8);

                        dot_acc = _mm256_add_epi32(dot_acc, _mm256_madd_epi16(a16, b16));
                        sx_acc = _mm256_add_epi32(sx_acc, _mm256_madd_epi16(a16, ones16));
                        sy_acc = _mm256_add_epi32(sy_acc, _mm256_madd_epi16(b16, ones16));
                    }

                    auto reduce_epi32_avx2 = [](__m256i v) -> int64_t {
                        __m128i lo = _mm256_castsi256_si128(v);
                        __m128i hi = _mm256_extracti128_si256(v, 1);
                        __m128i s = _mm_add_epi32(lo, hi);
                        s = _mm_hadd_epi32(s, s);
                        s = _mm_hadd_epi32(s, s);
                        return static_cast<int64_t>(_mm_cvtsi128_si32(s));
                    };

                    dot += reduce_epi32_avx2(dot_acc);
                    sum_xi += reduce_epi32_avx2(sx_acc);
                    sum_yi += reduce_epi32_avx2(sy_acc);
                }
#elif defined(USE_NEON) && defined(__ARM_FEATURE_DOTPROD)
                {
                    int32x4_t dot_acc = vdupq_n_s32(0);
                    int32x4_t sx_acc = vdupq_n_s32(0);
                    int32x4_t sy_acc = vdupq_n_s32(0);
                    const int8x16_t ones8 = vdupq_n_s8(1);

                    for(; i + 16 <= qty; i += 16) {
                        int8x16_t a8 = vld1q_s8(pVect1 + i);
                        int8x16_t b8 = vld1q_s8(pVect2 + i);
                        dot_acc = vdotq_s32(dot_acc, a8, b8);
                        sx_acc = vdotq_s32(sx_acc, a8, ones8);
                        sy_acc = vdotq_s32(sy_acc, b8, ones8);
                    }

                    dot += static_cast<int64_t>(vaddvq_s32(dot_acc));
                    sum_xi += static_cast<int64_t>(vaddvq_s32(sx_acc));
                    sum_yi += static_cast<int64_t>(vaddvq_s32(sy_acc));
                }
#elif defined(USE_SVE2)
                {
                    svint32_t dot_acc = svdup_s32(0);
                    svint32_t sx_acc = svdup_s32(0);
                    svint32_t sy_acc = svdup_s32(0);
                    const size_t vec_bytes = svcntb();
                    const svint8_t ones8 = svdup_s8(1);

                    for(; i + vec_bytes <= qty; i += vec_bytes) {
                        svint8_t a8 = svld1_s8(svptrue_b8(), pVect1 + i);
                        svint8_t b8 = svld1_s8(svptrue_b8(), pVect2 + i);
                        dot_acc = svdot_s32(dot_acc, a8, b8);
                        sx_acc = svdot_s32(sx_acc, a8, ones8);
                        sy_acc = svdot_s32(sy_acc, b8, ones8);
                    }

                    dot += static_cast<int64_t>(svaddv_s32(svptrue_b32(), dot_acc));
                    sum_xi += static_cast<int64_t>(svaddv_s32(svptrue_b32(), sx_acc));
                    sum_yi += static_cast<int64_t>(svaddv_s32(svptrue_b32(), sy_acc));
                }
#endif

                for(; i < qty; ++i) {
                    const int32_t a = static_cast<int32_t>(pVect1[i]);
                    const int32_t b = static_cast<int32_t>(pVect2[i]);
                    dot += static_cast<int64_t>(a) * b;
                    sum_xi += static_cast<int64_t>(a);
                    sum_yi += static_cast<int64_t>(b);
                }

                int64_t sum_ai_yi = 0;
                int64_t sum_bi_xi = 0;
                int64_t sum_ai_bi = 0;
                int64_t sum_pos_ai_yi = 0;
                int64_t sum_pos_bi_xi = 0;

                const size_t word_count = get_sign_word_count(qty);
                // Sign-mask-selected sums (vectorised per ISA; scalar gather as fallback).
#if defined(USE_AVX512)
                {
                    const __m512i ones_u8 = _mm512_set1_epi8(1);
                    __m512i accY = _mm512_setzero_si512(), accX = _mm512_setzero_si512();
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        accY = _mm512_dpbusd_epi32(accY, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b1, (const void*)(pVect2 + base)));
                        accX = _mm512_dpbusd_epi32(accX, ones_u8, _mm512_maskz_loadu_epi8((__mmask64)b2, (const void*)(pVect1 + base)));
                    }
                    sum_pos_ai_yi = _mm512_reduce_add_epi32(accY);
                    sum_pos_bi_xi = _mm512_reduce_add_epi32(accX);
                }
#elif defined(USE_AVX2)
                {
                    const __m256i ones16 = _mm256_set1_epi16(1);
                    __m256i accY = _mm256_setzero_si256(), accX = _mm256_setzero_si256();
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        size_t j = base;
                        for(; j + 16 <= end; j += 16) {
                            const __m128i MA = int8e_mask16_to_bytes((uint16_t)(b1 >> (j - base)));
                            const __m128i MB = int8e_mask16_to_bytes((uint16_t)(b2 >> (j - base)));
                            const __m128i y = _mm_loadu_si128((const __m128i*)(pVect2 + j));
                            const __m128i x = _mm_loadu_si128((const __m128i*)(pVect1 + j));
                            accY = _mm256_add_epi32(accY, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(y, MA)), ones16));
                            accX = _mm256_add_epi32(accX, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm_and_si128(x, MB)), ones16));
                        }
                        for(; j < end; ++j) {
                            const size_t bit = j - base;
                            if((b1 >> bit) & 1ULL) sum_pos_ai_yi += pVect2[j];
                            if((b2 >> bit) & 1ULL) sum_pos_bi_xi += pVect1[j];
                        }
                    }
                    sum_pos_ai_yi += int8e_hsum_epi32_avx2(accY);
                    sum_pos_bi_xi += int8e_hsum_epi32_avx2(accX);
                }
#elif defined(USE_NEON) && defined(__ARM_FEATURE_DOTPROD)
                {
                    const int8x16_t ones = vdupq_n_s8(1);
                    int32x4_t accY = vdupq_n_s32(0), accX = vdupq_n_s32(0);
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        size_t j = base;
                        for(; j + 16 <= end; j += 16) {
                            const int8x16_t MA = int8e_mask16_to_bytes_neon((uint16_t)(b1 >> (j - base)));
                            const int8x16_t MB = int8e_mask16_to_bytes_neon((uint16_t)(b2 >> (j - base)));
                            accY = vdotq_s32(accY, vandq_s8(vld1q_s8(pVect2 + j), MA), ones);
                            accX = vdotq_s32(accX, vandq_s8(vld1q_s8(pVect1 + j), MB), ones);
                        }
                        for(; j < end; ++j) {
                            const size_t bit = j - base;
                            if((b1 >> bit) & 1ULL) sum_pos_ai_yi += pVect2[j];
                            if((b2 >> bit) & 1ULL) sum_pos_bi_xi += pVect1[j];
                        }
                    }
                    sum_pos_ai_yi += vaddvq_s32(accY);
                    sum_pos_bi_xi += vaddvq_s32(accX);
                }
#elif defined(USE_SVE2)
                {
                    const svint8_t ones = svdup_s8(1);
                    const size_t vb = svcntb();
                    svint32_t accY = svdup_s32(0), accX = svdup_s32(0);
                    for(size_t w = 0; w < word_count; ++w) {
                        const size_t base = w * 64;
                        uint64_t mask = ~0ULL;
                        if(base + 64 > qty) {
                            const size_t remaining = qty - base;
                            mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                        }
                        const uint64_t b1 = bits1[w] & mask;
                        const uint64_t b2 = bits2[w] & mask;
                        const int64_t lane_count =
                                static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                        sum_ai_bi += lane_count
                                     - (static_cast<int64_t>(__builtin_popcountll(b1 ^ b2)) << 1);
                        const size_t end = std::min(base + 64, qty);
                        for(size_t o = 0; base + o < end; o += vb) {
                            svbool_t pg = svwhilelt_b8((uint64_t)(base + o), (uint64_t)end);
                            accY = svdot_s32(accY, svld1_s8(int8e_pred_from_bits(pg, b1 >> o), pVect2 + base + o), ones);
                            accX = svdot_s32(accX, svld1_s8(int8e_pred_from_bits(pg, b2 >> o), pVect1 + base + o), ones);
                        }
                    }
                    sum_pos_ai_yi += svaddv_s32(svptrue_b32(), accY);
                    sum_pos_bi_xi += svaddv_s32(svptrue_b32(), accX);
                }
#else
                for(size_t w = 0; w < word_count; ++w) {
                    const size_t base = w * 64;
                    uint64_t mask = ~0ULL;
                    if(base + 64 > qty) {
                        const size_t remaining = qty - base;
                        mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                    }

                    const uint64_t b1 = bits1[w] & mask;
                    const uint64_t b2 = bits2[w] & mask;

                    const int64_t lane_count =
                            static_cast<int64_t>((base + 64 <= qty) ? 64 : (qty - base));
                    const int64_t diff = static_cast<int64_t>(__builtin_popcountll(b1 ^ b2));
                    sum_ai_bi += lane_count - (diff << 1);

                    uint64_t active1 = b1;
                    while(active1 != 0ULL) {
                        const size_t bit = static_cast<size_t>(__builtin_ctzll(active1));
                        const size_t idx = base + bit;
                        sum_pos_ai_yi += static_cast<int64_t>(pVect2[idx]);
                        active1 &= (active1 - 1ULL);
                    }

                    uint64_t active2 = b2;
                    while(active2 != 0ULL) {
                        const size_t bit = static_cast<size_t>(__builtin_ctzll(active2));
                        const size_t idx = base + bit;
                        sum_pos_bi_xi += static_cast<int64_t>(pVect1[idx]);
                        active2 &= (active2 - 1ULL);
                    }
                }
#endif

                sum_ai_yi = (sum_pos_ai_yi << 1) - sum_yi;
                sum_bi_xi = (sum_pos_bi_xi << 1) - sum_xi;

                const float s1s2 = scale1 * scale2;
                const float base = static_cast<float>(dot) * s1s2;
                const float correction =
                        0.25f * static_cast<float>(sum_ai_yi + sum_bi_xi) * s1s2;
                const float quadratic = 0.0625f * static_cast<float>(sum_ai_bi) * s1s2;
                return base + correction + quadratic;
            }

            static float
            InnerProduct(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                return 1.0f - InnerProductSim(pVect1v, pVect2v, qty_ptr);
            }

            static float CosineSim(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                return InnerProductSim(pVect1v, pVect2v, qty_ptr);
            }

            static float Cosine(const void* pVect1v, const void* pVect2v, const void* qty_ptr) {
                return 1.0f - CosineSim(pVect1v, pVect2v, qty_ptr);
            }

            static void L2SqrSimBatch(const void* query,
                                      const void* const* vectors,
                                      size_t count,
                                      const void* params,
                                      float* out) {
                for(size_t i = 0; i < count; ++i) {
                    out[i] = L2SqrSim(query, vectors[i], params);
                }
            }

            static void InnerProductSimBatch(const void* query,
                                             const void* const* vectors,
                                             size_t count,
                                             const void* params,
                                             float* out) {
                for(size_t i = 0; i < count; ++i) {
                    out[i] = InnerProductSim(query, vectors[i], params);
                }
            }

            static void CosineSimBatch(const void* query,
                                       const void* const* vectors,
                                       size_t count,
                                       const void* params,
                                       float* out) {
                for(size_t i = 0; i < count; ++i) {
                    out[i] = CosineSim(query, vectors[i], params);
                }
            }

            static std::vector<uint8_t> quantize_to_int8(const void* in, size_t dim) {
                const uint8_t* src = static_cast<const uint8_t*>(in);
                std::vector<uint8_t> out(int8::get_storage_size(dim));

                std::copy(src, src + dim * sizeof(int8_t), out.data());

                const float scale = extract_scale(src, dim);
                float* scale_ptr = reinterpret_cast<float*>(out.data() + dim * sizeof(int8_t));
                *scale_ptr = scale;

                return out;
            }

        }  // namespace int8e

        class Int8EQuantizer : public ndd::quant::Quantizer {
        public:
            std::string name() const override { return "int8e"; }
            ndd::quant::QuantizationLevel level() const override {
                return ndd::quant::QuantizationLevel::INT8E;
            }

            ndd::quant::QuantizerDispatch getDispatch() const override {
                ndd::quant::QuantizerDispatch d;
                d.dist_l2 = &int8e::L2Sqr;
                d.dist_ip = &int8e::InnerProduct;
                d.dist_cosine = &int8e::Cosine;
                d.sim_l2 = &int8e::L2SqrSim;
                d.sim_ip = &int8e::InnerProductSim;
                d.sim_cosine = &int8e::CosineSim;
                d.sim_l2_batch = &int8e::L2SqrSimBatch;
                d.sim_ip_batch = &int8e::InnerProductSimBatch;
                d.sim_cosine_batch = &int8e::CosineSimBatch;
                d.quantize = &int8e::quantize;
                d.dequantize = &int8e::dequantize;
                d.quantize_to_int8 = &int8e::quantize_to_int8;
                d.get_storage_size = &int8e::get_storage_size;
                d.extract_scale = &int8e::extract_scale;
                return d;
            }
        };

        static ndd::quant::RegisterQuantizer reg_int8e(ndd::quant::QuantizationLevel::INT8E,
                                                        "int8e",
                                                        std::make_shared<Int8EQuantizer>());

    }  // namespace quant
}  // namespace ndd
