#pragma once
#include "bit_ops.h"
#include "../set_ops/avx2.h"
#include "../set_ops/neon.h"

namespace minigraph::bit_ops {
namespace internal {
inline size_t mark_tail(const uint32_t *a, size_t na, const uint32_t *b, size_t nb,
                        Word *out, size_t i = 0, size_t j = 0) {
    size_t count = 0;
    while (i < na && j < nb) {
        const auto x = a[i], y = b[j];
        if (x == y) {
            out[i / 64] |= Word{1} << (i % 64);
            ++count;
        }
        i += x <= y;
        j += y <= x;
    }
    return count;
}
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
// LOVE's mark-intersection approach: emit positions in A, not matching IDs.
__attribute__((target("avx2"))) inline size_t mark_avx2(
    const uint32_t *a, size_t na, const uint32_t *b, size_t nb, Word *out) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 8 && nb - j >= 8) {
        const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i));
        const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + j));
        const unsigned mask = set_ops::avx2_match_mask(va, vb);
        // A block can match several B blocks. OR their masks; unique input
        // IDs ensure their counted matches are disjoint. Eight lanes cannot
        // cross a word boundary because i advances only in multiples of 8.
        out[i / 64] |= Word{mask} << (i % 64);
        count += popcount(mask);
        const auto am = a[i + 7], bm = b[j + 7];
        i += (am <= bm) * 8;
        j += (bm <= am) * 8;
    }
    return count + mark_tail(a, na, b, nb, out, i, j);
}
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
inline size_t mark_neon(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, Word *out) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 4 && nb - j >= 4) {
        const unsigned mask = set_ops::neon_match_mask(vld1q_u32(a + i), vld1q_u32(b + j));
        // Four-lane blocks are word-aligned in position space, just as above.
        out[i / 64] |= Word{mask} << (i % 64);
        count += popcount(mask);
        const auto am = a[i + 3], bm = b[j + 3];
        i += (am <= bm) * 4;
        j += (bm <= am) * 4;
    }
    return count + mark_tail(a, na, b, nb, out, i, j);
}
#endif
} // namespace internal

// Sorted unique uint32 IDs; bit i represents membership of universe[i] in ids.
// Overwrites exactly word_count(n) words, clears tail bits, returns cardinality.
// No padding/alignment beyond element alignment; output must not alias inputs.
// Null pointers are allowed only for zero-length buffers. A is never swapped
// with B: the output coordinate system must remain the universe's positions.
template <bool Simd = true>
inline size_t from_sorted(const uint32_t *universe, size_t n, const uint32_t *ids,
                         size_t size, Word *out) {
    for (size_t k = 0; k < word_count(n); ++k)
        out[k] = 0;
    if (!n || !size)
        return 0;
    if constexpr (Simd) {
#if !defined(GRAPHMINI_BIT_OPS_SCALAR) && !defined(GRAPHMINI_BIT_OPS_PORTABLE)
#if defined(__aarch64__) || defined(_M_ARM64)
        if (n >= 4 && size >= 4)
            return internal::mark_neon(universe, n, ids, size, out);
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
        if (n >= 8 && size >= 8 && internal::has_avx2())
            return internal::mark_avx2(universe, n, ids, size, out);
#endif
#endif
    }
    return internal::mark_tail(universe, n, ids, size, out);
}
} // namespace minigraph::bit_ops
