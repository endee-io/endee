#include "cpu_runtime_dispatch.hpp"

#include <mutex>
#include <sstream>

#include "check_avx_compat.hpp"
#include "../log.hpp"

namespace {
std::once_flag g_dispatch_init_once;
ndd::cpu::X86SimdCaps g_active_x86_caps;
bool g_dispatch_initialized = false;
bool g_runtime_x86_compatible = false;
}  // namespace

namespace ndd::cpu {

X86SimdCaps probe_x86_simd_caps() {
    X86SimdCaps caps;

#if defined(__x86_64__) || defined(_M_X64)
    caps.os_avx = os_supports_avx();
    caps.os_avx512_state = os_supports_avx512_state();
    caps.avx2 = cpu_has_avx2();
    caps.avx512f = cpu_has_avx512f();
    caps.avx512dq = cpu_has_avx512dq();
    caps.avx512bw = cpu_has_avx512bw();
    caps.avx512vnni = cpu_has_avx512vnni();
    caps.avx512fp16 = cpu_has_avx512f_and_fp16();
    caps.avx512vpopcntdq = cpu_has_avx512vpopcntdq();
#endif

    return caps;
}

X86SimdCaps compute_active_x86_simd_caps(const X86SimdCaps& detected) {
    X86SimdCaps active;
    active.os_avx = detected.os_avx;
    active.os_avx512_state = detected.os_avx512_state;

#if defined(__x86_64__) || defined(_M_X64)
    if(!(detected.avx2 && detected.os_avx)) {
        return active;
    }

    active.avx2 = true;

    if(!(detected.avx512f && detected.avx512dq && detected.os_avx512_state)) {
        return active;
    }

    active.avx512f = true;
    active.avx512dq = true;
    active.avx512bw = detected.avx512bw;
    active.avx512vnni = detected.avx512vnni;
    active.avx512fp16 = detected.avx512fp16;
    active.avx512vpopcntdq = detected.avx512vpopcntdq;
#else
    (void)detected;
#endif

    return active;
}

void bind_x86_dispatch(const X86SimdCaps& detected) {
    g_active_x86_caps = compute_active_x86_simd_caps(detected);
    g_dispatch_initialized = true;
}

static std::string join_flags(const std::vector<std::string>& flags) {
    std::ostringstream oss;
    for(size_t i = 0; i < flags.size(); ++i) {
        if(i != 0) {
            oss << ", ";
        }
        oss << flags[i];
    }
    return oss.str();
}

bool initialize_cpu_dispatch() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    std::call_once(g_dispatch_init_once, []() {
        const X86SimdCaps detected = probe_x86_simd_caps();
        bind_x86_dispatch(detected);

        if(!g_active_x86_caps.avx2) {
            if(!detected.avx2) {
                LOG_ERROR("Runtime x86 dispatch requires AVX2 support on the host CPU");
            } else if(!detected.os_avx) {
                LOG_ERROR("Runtime x86 dispatch requires AVX state support from the OS");
            } else {
                LOG_ERROR("Runtime x86 dispatch could not enable the AVX2 baseline");
            }
            g_runtime_x86_compatible = false;
            return;
        }

        g_runtime_x86_compatible = true;
        LOG_INFO("Runtime x86 dispatch active with CPU flags: "
                 << join_flags(serialize_active_cpu_flags(g_active_x86_caps)));

        if(detected.avx512f && detected.os_avx512_state) {
            std::vector<std::string> downgraded;
            if(!g_active_x86_caps.avx512dq) {
                downgraded.push_back("avx512dq");
            }
            if(!g_active_x86_caps.avx512bw) {
                downgraded.push_back("avx512bw");
            }
            if(!g_active_x86_caps.avx512vnni) {
                downgraded.push_back("avx512vnni");
            }
            if(!g_active_x86_caps.avx512fp16) {
                downgraded.push_back("avx512fp16");
            }
            if(!g_active_x86_caps.avx512vpopcntdq) {
                downgraded.push_back("avx512vpopcntdq");
            }
            if(!downgraded.empty()) {
                LOG_WARN("Runtime x86 dispatch is falling back to AVX2 for unsupported AVX512 "
                         "subextensions: "
                         << join_flags(downgraded));
            }
        } else {
            LOG_INFO("Runtime x86 dispatch is using the AVX2 baseline only");
        }
    });

    return g_runtime_x86_compatible;
#else
    return true;
#endif
}

const X86SimdCaps& get_active_x86_simd_caps() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    initialize_cpu_dispatch();
#endif
    return g_active_x86_caps;
}

std::vector<std::string> serialize_active_cpu_flags(const X86SimdCaps& caps) {
    std::vector<std::string> flags;

    if(caps.avx2) {
        flags.push_back("avx2");
    }
    if(caps.avx512f) {
        flags.push_back("avx512f");
    }
    if(caps.avx512dq) {
        flags.push_back("avx512dq");
    }
    if(caps.avx512bw) {
        flags.push_back("avx512bw");
    }
    if(caps.avx512vnni) {
        flags.push_back("avx512vnni");
    }
    if(caps.avx512fp16) {
        flags.push_back("avx512fp16");
    }
    if(caps.avx512vpopcntdq) {
        flags.push_back("avx512vpopcntdq");
    }

    return flags;
}

std::vector<std::string> get_default_active_cpu_flags() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return serialize_active_cpu_flags(get_active_x86_simd_caps());
#elif defined(USE_AVX512)
    return {"avx2",
            "avx512f",
            "avx512dq",
            "avx512bw",
            "avx512vnni",
            "avx512fp16",
            "avx512vpopcntdq"};
#elif defined(USE_AVX2)
    return {"avx2"};
#elif defined(USE_SVE2)
    return {"sve2"};
#elif defined(USE_NEON)
    return {"neon"};
#else
    return {};
#endif
}

std::vector<std::string> get_active_cpu_flags() { return get_default_active_cpu_flags(); }

bool use_avx2() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx2;
#else
    return false;
#endif
}

bool use_avx512f() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512f;
#else
    return false;
#endif
}

bool use_avx512dq() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512dq;
#else
    return false;
#endif
}

bool use_avx512bw() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512bw;
#else
    return false;
#endif
}

bool use_avx512vnni() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512vnni;
#else
    return false;
#endif
}

bool use_avx512fp16() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512fp16;
#else
    return false;
#endif
}

bool use_avx512vpopcntdq() {
#if defined(NDD_RUNTIME_X86_DISPATCH) && (defined(__x86_64__) || defined(_M_X64))
    return get_active_x86_simd_caps().avx512vpopcntdq;
#else
    return false;
#endif
}

}  // namespace ndd::cpu
