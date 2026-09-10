#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
namespace minigraph::bit_ops::internal {
inline bool has_avx2() {
    static const bool supported = __builtin_cpu_supports("avx2");
    return supported;
}

// Full groups of four uint64 words only. Callers handle partial words/tails.
// Unaligned accesses preserve the exact-length, uint64-aligned buffer contract.
template <bool Subtract, bool Write, bool Count = true>
__attribute__((target("avx2"))) inline size_t avx2_words(const uint64_t *a, const uint64_t *b,
                                                         size_t words, uint64_t *out) {
    const auto lookup = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1, 2, 1,
                                         2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const auto nibble = _mm256_set1_epi8(15);
    const auto zero = _mm256_setzero_si256();
    auto totals = zero;
    for (size_t i = 0; i < words; i += 4) {
        const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i));
        const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
        // ANDNOT(x,y) means ~x & y, hence RHS first for A minus B.
        const auto value = Subtract ? _mm256_andnot_si256(right, left) : _mm256_and_si256(left, right);
        if constexpr (Write)
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i), value);
        // AVX2 has no vector popcount: count low/high nibbles with byte
        // shuffles, then widen immediately to avoid byte accumulator overflow.
        if constexpr (Count) {
            const auto lo = _mm256_and_si256(value, nibble);
            const auto hi = _mm256_and_si256(_mm256_srli_epi16(value, 4), nibble);
            const auto bytes =
                _mm256_add_epi8(_mm256_shuffle_epi8(lookup, lo), _mm256_shuffle_epi8(lookup, hi));
            totals = _mm256_add_epi64(totals, _mm256_sad_epu8(bytes, zero));
        }
    }
    if constexpr (!Count) return 0;
    const auto halves =
        _mm_add_epi64(_mm256_castsi256_si128(totals), _mm256_extracti128_si256(totals, 1));
    return static_cast<size_t>(_mm_cvtsi128_si64(halves)) +
           static_cast<size_t>(_mm_extract_epi64(halves, 1));
}
} // namespace minigraph::bit_ops::internal
#endif
