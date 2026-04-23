#pragma once

#include <string>
#include <vector>

namespace ndd::cpu {

struct X86SimdCaps {
    bool avx2 = false;
    bool avx512f = false;
    bool avx512dq = false;
    bool avx512bw = false;
    bool avx512vnni = false;
    bool avx512fp16 = false;
    bool avx512vpopcntdq = false;
    bool os_avx = false;
    bool os_avx512_state = false;
};

X86SimdCaps probe_x86_simd_caps();
X86SimdCaps compute_active_x86_simd_caps(const X86SimdCaps& detected);
void bind_x86_dispatch(const X86SimdCaps& detected);
bool initialize_cpu_dispatch();

const X86SimdCaps& get_active_x86_simd_caps();
std::vector<std::string> get_active_cpu_flags();
std::vector<std::string> serialize_active_cpu_flags(const X86SimdCaps& caps);
std::vector<std::string> get_default_active_cpu_flags();

bool use_avx2();
bool use_avx512f();
bool use_avx512dq();
bool use_avx512bw();
bool use_avx512vnni();
bool use_avx512fp16();
bool use_avx512vpopcntdq();

}  // namespace ndd::cpu

#if defined(USE_AVX512) || defined(NDD_COMPILE_AVX512_VARIANTS)
#    define NDD_HAS_AVX512_VARIANTS 1
#else
#    define NDD_HAS_AVX512_VARIANTS 0
#endif

#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__GNUC__) || defined(__clang__))
#    define NDD_TARGET_ATTR(features) __attribute__((target(features), noinline))
#else
#    define NDD_TARGET_ATTR(features)
#endif

#define NDD_TARGET_AVX512F NDD_TARGET_ATTR("avx512f,avx512dq")
#define NDD_TARGET_AVX512BW NDD_TARGET_ATTR("avx512f,avx512dq,avx512bw")
#define NDD_TARGET_AVX512VNNI NDD_TARGET_ATTR("avx512f,avx512dq,avx512vnni")
#define NDD_TARGET_AVX512BW_VNNI NDD_TARGET_ATTR("avx512f,avx512dq,avx512bw,avx512vnni")
#define NDD_TARGET_AVX512FP16 NDD_TARGET_ATTR("avx512f,avx512dq,avx512fp16")
#define NDD_TARGET_AVX512VPOPCNTDQ NDD_TARGET_ATTR("avx512f,avx512dq,avx512vpopcntdq")
