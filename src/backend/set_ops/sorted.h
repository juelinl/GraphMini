#pragma once
#include <cstddef>
#include <cstdint>
#include "neon.h"
#include "avx2.h"
#include <algorithm>
#include <cstring>

namespace minigraph::set_ops {
inline const uint32_t* advance_to(const uint32_t* begin, const uint32_t* end, uint32_t value) {
    while (begin != end && *begin < value) ++begin;
    return begin;
}
// Sorted unique uint32 arrays, no padding/alignment requirements. Empty inputs
// may be null. Write outputs must not alias inputs and need only the result size.
inline size_t lower_bound_binary(const uint32_t* a, size_t n, uint32_t value) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < value) lo = mid + 1; else hi = mid;
    }
    return lo;
}
inline size_t lower_bound_linear(const uint32_t* a, size_t n, uint32_t value) {
    size_t i = 0;
    while (i < n && a[i] < value) ++i;
    return i;
}
inline size_t indices_scalar(const uint32_t* a, size_t na, const uint32_t* b,
                             size_t nb, uint32_t* out, size_t base = 0) {
    size_t i = 0, j = 0, count = 0;
    while (i < na && j < nb) {
        const auto left = a[i], right = b[j];
        if (left == right) out[count++] = static_cast<uint32_t>(base + i);
        i += left <= right;
        j += right <= left;
    }
    return count;
}
inline size_t emit_indices(size_t base, unsigned mask, uint32_t* out) {
    size_t count = 0;
    while (mask) {
        const unsigned lane = __builtin_ctz(mask);
        out[count++] = static_cast<uint32_t>(base + lane);
        mask &= mask - 1;
    }
    return count;
}
#if defined(__aarch64__) || defined(_M_ARM64)
inline size_t lower_bound_simd(const uint32_t* a, size_t n, uint32_t value) {
    size_t i = 0;
    for (; n - i >= 4; i += 4) {
        const unsigned mask = neon_lane_mask(vcgeq_u32(vld1q_u32(a + i), vdupq_n_u32(value)));
        if (mask) return i + __builtin_ctz(mask);
    }
    return i < n ? i + lower_bound_linear(a + i, n - i, value) : i;
}
inline size_t indices_simd(const uint32_t* a, size_t na, const uint32_t* b,
                           size_t nb, uint32_t* out) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 4 && nb - j >= 4) {
        const auto mask = neon_match_mask(vld1q_u32(a + i), vld1q_u32(b + j));
        if (mask) count += emit_indices(i, mask, out + count);
        const auto am = a[i + 3], bm = b[j + 3];
        i += (am <= bm) * 4; j += (bm <= am) * 4;
    }
    if (i < na && j < nb) count += indices_scalar(a + i, na - i, b + j, nb - j, out ? out + count : nullptr, i);
    return count;
}
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
__attribute__((target("avx2"))) inline size_t lower_bound_simd(const uint32_t* a, size_t n, uint32_t value) {
    size_t i = 0;
    const auto sign = _mm256_set1_epi32(INT32_MIN);
    const auto target = _mm256_xor_si256(_mm256_set1_epi32(static_cast<int32_t>(value)), sign);
    for (; n - i >= 8; i += 8) {
        const auto data = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i)), sign);
        const unsigned mask = (~avx2_lane_mask(_mm256_cmpgt_epi32(target, data))) & 255u;
        if (mask) return i + __builtin_ctz(mask);
    }
    return i < n ? i + lower_bound_linear(a + i, n - i, value) : i;
}
__attribute__((target("avx2"))) inline size_t indices_simd(const uint32_t* a, size_t na,
                        const uint32_t* b, size_t nb, uint32_t* out) {
    size_t i = 0, j = 0, count = 0;
    while (na - i >= 8 && nb - j >= 8) {
        const auto mask = avx2_match_mask(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + j)));
        if (mask) count += emit_indices(i, mask, out + count);
        const auto am = a[i + 7], bm = b[j + 7];
        i += (am <= bm) * 8; j += (bm <= am) * 8;
    }
    if (i < na && j < nb) count += indices_scalar(a + i, na - i, b + j, nb - j, out ? out + count : nullptr, i);
    return count;
}
#else
inline size_t lower_bound_simd(const uint32_t* a, size_t n, uint32_t v) { return lower_bound_linear(a, n, v); }
inline size_t indices_simd(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) {
    return indices_scalar(a, na, b, nb, out);
}
#endif
inline bool sorted_simd_available() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
    return has_avx2();
#else
    return false;
#endif
}
inline size_t lower_bound_index(const uint32_t* a, size_t n, uint32_t value) {
    // Preserve the existing search policy until benchmarks justify a change.
    return n <= 64 ? lower_bound_linear(a, n, value) : lower_bound_binary(a, n, value);
}
inline size_t remove_count(const uint32_t* a, size_t n, uint32_t value) {
    const size_t i = lower_bound_index(a, n, value);
    return n - (i < n && a[i] == value);
}
// Caller has already found the matching position. No second search.
inline void remove_at(const uint32_t* a, size_t n, size_t i, uint32_t* out) {
    if (i) std::memcpy(out, a, i * sizeof(uint32_t));
    if (n - i > 1) std::memcpy(out + i, a + i + 1, (n - i - 1) * sizeof(uint32_t));
}
inline size_t indices_write(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) {
    if (!na || !nb) return 0;
    if (a == b) {
        const size_t n = std::min(na, nb);
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint32_t>(i);
        return n;
    }
    // Sparse right input: monotonic binary searches into the left array.
    // Division avoids overflowing nb * 50. Require left indices to fit uint32.
    if (nb <= (na - 1) / 50) {
        size_t i = 0, count = 0;
        for (size_t j = 0; j < nb && i < na; ++j) {
            i += lower_bound_binary(a + i, na - i, b[j]);
            if (i < na && a[i] == b[j]) out[count++] = static_cast<uint32_t>(i++);
        }
        return count;
    }
    if (na >= 32 && nb >= 32 && sorted_simd_available()) return indices_simd(a, na, b, nb, out);
    return indices_scalar(a, na, b, nb, out);
}
}
