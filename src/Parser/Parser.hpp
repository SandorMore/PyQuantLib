#include <immintrin.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <exception>
#include <memory>
#include <new>
#include <bitset>
#include <cstdint>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

enum class DIA {
    NONE,      
    SSE4_2,
    AVX2,
    AVX512,
    NEON       
};

namespace detail {

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))

inline DIA detect_x86_msvc() {
    int r[4];                       // eax, ebx, ecx, edx
    __cpuid(r, 0);
    const int max_leaf = r[0];
    if (max_leaf < 1) return DIA::NONE;

    __cpuid(r, 1);
    const bool sse42    = r[2] & (1 << 20);
    const bool osxsave  = r[2] & (1 << 27);
    const bool avx      = r[2] & (1 << 28);

    uint64_t xcr0 = 0;
    if (osxsave) xcr0 = _xgetbv(0);
    const bool os_ymm = (xcr0 & 0x06) == 0x06;   // XMM + YMM state enabled
    const bool os_zmm = (xcr0 & 0xE6) == 0xE6;   // + opmask, ZMM_Hi256, Hi16_ZMM

    bool avx2 = false, avx512 = false;
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        avx2 = r[1] & (1 << 5);
        const bool f  = r[1] & (1 << 16);
        const bool dq = r[1] & (1 << 17);
        const bool bw = r[1] & (1 << 30);
        const bool vl = r[1] & (1 << 31);
        avx512 = f && dq && bw && vl;
    }

    if (avx512 && os_zmm && avx && os_ymm) return DIA::AVX512;
    if (avx2   && avx && os_ymm)           return DIA::AVX2;
    if (sse42)                             return DIA::SSE4_2;
    return DIA::NONE;
}

#endif

} // namespace detail

inline DIA run_diagnostics()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return DIA::NEON;   // NEON is mandatory on AArch64

#elif defined(__x86_64__) || defined(__i386__)
  #if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    // GCC/Clang's runtime also verifies OS register-state support (XGETBV)
    if (__builtin_cpu_supports("avx512f")  && __builtin_cpu_supports("avx512dq") &&
        __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl"))
        return DIA::AVX512;
    if (__builtin_cpu_supports("avx2"))   return DIA::AVX2;
    if (__builtin_cpu_supports("sse4.2")) return DIA::SSE4_2;
    return DIA::NONE;
  #elif defined(_MSC_VER)
    return detail::detect_x86_msvc();
  #endif

#else
    return DIA::NONE;
#endif
}

inline const char* to_string(DIA d) {
    switch (d) {
        case DIA::AVX512: return "AVX-512";
        case DIA::AVX2:   return "AVX2";
        case DIA::SSE4_2: return "SSE4.2";
        case DIA::NEON:   return "NEON";
        default:          return "scalar";
    }
}

inline DIA active_arch();

void init();

namespace avx2
{
    struct State 
    {
        uint64_t prev_escaped;
        uint64_t prev_in_string;
        uint64_t prev_scalar;
    };    

} // namespace avx2
