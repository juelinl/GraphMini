#pragma once
#include <cstddef>
#include <cstdint>

namespace minigraph::set_ops {
// Inputs are sorted, unique uint32_t arrays. Output must not alias either input.
// No padding or alignment is required; write kernels store exactly count values.
template<bool Write, bool Bounded = false>
inline size_t scalar(const uint32_t* a, size_t na, const uint32_t* b, size_t nb,
                     uint32_t* out = nullptr, size_t* consumed_a = nullptr,
                     size_t* consumed_b = nullptr, uint32_t upper = 0) {
    size_t i = 0, j = 0, count = 0;
    while (i < na && j < nb) {
        const auto x = a[i], y = b[j];
        if constexpr (Bounded) {
            if (x >= upper || y >= upper) break;
        }
        i += x <= y;
        j += y <= x;
        if (x == y) {
            if constexpr (Write) out[count] = x;
            ++count;
        }
    }
    if (consumed_a) *consumed_a = i;
    if (consumed_b) *consumed_b = j;
    return count;
}

// pending marks leading left-hand elements already matched by earlier SIMD
// right-hand blocks. It must survive the transition to the scalar tail.
template<bool Write, bool Bounded = false>
inline size_t difference_scalar(const uint32_t* a, size_t na,
                                const uint32_t* b, size_t nb, uint32_t excluded,
                                uint32_t* out = nullptr, unsigned pending = 0,
                                uint32_t upper = 0) {
    size_t i = 0, j = 0, count = 0;
    while (i < na && j < nb) {
        const auto x = a[i], y = b[j];
        if constexpr (Bounded) {
            if (x >= upper || y >= upper) break;
        }
        if (x < y && x != excluded && !(pending & 1u)) {
            if constexpr (Write) out[count] = x;
            ++count;
        }
        if (x <= y) { ++i; pending >>= 1; }
        if (y <= x) ++j;
    }
    while (i < na) {
        const auto x = a[i++];
        if constexpr (Bounded) {
            if (x >= upper) break;
        }
        if (x != excluded && !(pending & 1u)) {
            if constexpr (Write) out[count] = x;
            ++count;
        }
        pending >>= 1;
    }
    return count;
}

template<bool Write>
inline size_t emit_mask(const uint32_t* a, unsigned mask, uint32_t* out) {
    if constexpr (Write) {
        size_t n = 0;
        while (mask) {
            const unsigned lane = __builtin_ctz(mask);
            out[n++] = a[lane];
            mask &= mask - 1;
        }
        return n;
    } else {
        return __builtin_popcount(mask);
    }
}
}
