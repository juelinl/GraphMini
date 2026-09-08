#pragma once
#include "scalar.h"
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
namespace minigraph::set_ops {
inline bool has_avx2() {
    static const bool supported = __builtin_cpu_supports("avx2");
    return supported;
}

template<bool Write>
__attribute__((target("avx2"))) inline size_t avx2(
    const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
    uint32_t* out = nullptr) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 8 && nb - j >= 8) {
        const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + j));
        auto matches = _mm256_cmpeq_epi32(va, vb);
        const auto lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        for (int shift = 1; shift < 8; ++shift) {
            const auto indices = _mm256_add_epi32(lanes, _mm256_set1_epi32(shift));
            matches = _mm256_or_si256(matches,
                _mm256_cmpeq_epi32(va, _mm256_permutevar8x32_epi32(vb, indices)));
        }
        const unsigned mask = _mm256_movemask_ps(_mm256_castsi256_ps(matches));
        count += emit_mask<Write>(a + i, mask, Write && out ? out + count : nullptr);
        const auto am = a[i + 7], bm = b[j + 7];
        i += (am <= bm) * 8;
        j += (bm <= am) * 8;
    }
    if (i < na && j < nb)
        count += scalar<Write>(a + i, na - i, b + j, nb - j,
                               Write && out ? out + count : nullptr);
    return count;
}
}
#endif
