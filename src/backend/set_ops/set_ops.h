#pragma once
#include "scalar.h"
#include "neon.h"
#include "avx2.h"
#include "sorted.h"

namespace minigraph::set_ops {
// Conservative initial cutoff, not a universal crossover. Keep short candidate
// lists on the scalar path; benchmark before changing this policy.
inline constexpr size_t simd_min_size = 32;

template<bool Write>
inline size_t intersection(const uint32_t* a, size_t na,
                           const uint32_t* b, size_t nb, uint32_t* out = nullptr) {
    if (na >= simd_min_size && nb >= simd_min_size) {
#if defined(__aarch64__) || defined(_M_ARM64)
        return neon<Write>(a, na, b, nb, out);
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
        if (has_avx2()) return avx2<Write>(a, na, b, nb, out);
#endif
    }
    return scalar<Write>(a, na, b, nb, out);
}
inline size_t intersection_count(const uint32_t* a, size_t na,
                                 const uint32_t* b, size_t nb) {
    return intersection<false>(a, na, b, nb);
}
inline size_t intersection_write(const uint32_t* a, size_t na,
                                 const uint32_t* b, size_t nb, uint32_t* out) {
    return intersection<true>(a, na, b, nb, out);
}
// GraphMini difference is A minus B minus {excluded}; excluded is mandatory,
// including when it equals UINT32_MAX (not a "no exclusion" sentinel).
template<bool Write>
inline size_t difference(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                         uint32_t excluded, uint32_t* out = nullptr) {
    if (na >= simd_min_size && nb >= simd_min_size) {
#if defined(__aarch64__) || defined(_M_ARM64)
        return difference_neon<Write>(a, na, b, nb, excluded, out);
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
        if (has_avx2()) return difference_avx2<Write>(a, na, b, nb, excluded, out);
#endif
    }
    return difference_scalar<Write>(a, na, b, nb, excluded, out);
}
inline size_t difference_count(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                               uint32_t excluded) {
    return difference<false>(a, na, b, nb, excluded);
}
inline size_t difference_write(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                               uint32_t excluded, uint32_t* out) {
    return difference<true>(a, na, b, nb, excluded, out);
}
// Prefix length with IDs strictly below upper. Avoid pointer arithmetic on null
// empty views, and preserve unsigned ordering across the full uint32_t range.
inline size_t prefix_size(const uint32_t* a, size_t n, uint32_t upper) {
    return lower_bound_binary(a, n, upper);
}
template<bool Write>
inline size_t intersection_bounded(const uint32_t* a, size_t na,
                                  const uint32_t* b, size_t nb,
                                  uint32_t upper, uint32_t* out = nullptr) {
    if (na < simd_min_size || nb < simd_min_size)
        return scalar<Write, true>(a, na, b, nb, out, nullptr, nullptr, upper);
    return intersection<Write>(a, prefix_size(a, na, upper),
                               b, prefix_size(b, nb, upper), out);
}
template<bool Write>
inline size_t difference_bounded(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                                uint32_t excluded, uint32_t upper, uint32_t* out = nullptr) {
    if (na < simd_min_size || nb < simd_min_size)
        return difference_scalar<Write, true>(a, na, b, nb, excluded, out, 0, upper);
    return difference<Write>(a, prefix_size(a, na, upper),
                             b, prefix_size(b, nb, upper), excluded, out);
}
}
