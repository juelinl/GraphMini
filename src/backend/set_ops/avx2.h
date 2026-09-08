#pragma once
#include "scalar.h"
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
namespace minigraph::set_ops {
inline bool has_avx2() {
    static const bool supported = __builtin_cpu_supports("avx2");
    return supported;
}

__attribute__((target("avx2"))) inline unsigned avx2_lane_mask(__m256i matches) {
    return _mm256_movemask_ps(_mm256_castsi256_ps(matches));
}
__attribute__((target("avx2"))) inline unsigned avx2_match_mask(__m256i va, __m256i vb) {
    auto matches = _mm256_cmpeq_epi32(va, vb);
    const auto lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (int shift = 1; shift < 8; ++shift) {
        const auto indices = _mm256_add_epi32(lanes, _mm256_set1_epi32(shift));
        matches = _mm256_or_si256(matches,
            _mm256_cmpeq_epi32(va, _mm256_permutevar8x32_epi32(vb, indices)));
    }
    return avx2_lane_mask(matches);
}

template<bool Write>
__attribute__((target("avx2"))) inline size_t avx2(
    const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
    uint32_t* out = nullptr) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 8 && nb - j >= 8) {
        const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + j));
        const unsigned mask = avx2_match_mask(va, vb);
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
template<bool Write>
__attribute__((target("avx2"))) inline size_t difference_avx2(
    const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
    uint32_t excluded, uint32_t* out = nullptr) {
    size_t i = 0, j = 0, count = 0;
    unsigned pending = 0;
    while (na - i >= 8 && nb - j >= 8) {
        const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + j));
        pending |= avx2_match_mask(va, vb);
        const auto am = a[i + 7], bm = b[j + 7];
        // Do not complement each pair's mask: this left block can still match
        // a later right block. Emit only once all relevant blocks are visited.
        if (am <= bm) {
            pending |= avx2_lane_mask(
                _mm256_cmpeq_epi32(va, _mm256_set1_epi32(static_cast<int>(excluded))));
            count += emit_mask<Write>(a + i, (~pending) & 0xffu,
                                     Write && out ? out + count : nullptr);
            i += 8;
            pending = 0;
        }
        if (bm <= am) j += 8;
    }
    if (i < na)
        count += difference_scalar<Write>(a + i, na - i, b ? b + j : nullptr, nb - j,
            excluded, Write && out ? out + count : nullptr, pending);
    return count;
}

}
#endif
