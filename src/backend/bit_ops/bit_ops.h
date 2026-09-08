#pragma once
#include "avx2.h"
#include "neon.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace minigraph::bit_ops {
using Word = uint64_t;
static_assert(sizeof(void *) == 8, "GraphMini bitmap runtime requires a 64-bit platform");
inline constexpr size_t word_bits = 64;
inline constexpr size_t unlimited = std::numeric_limits<size_t>::max();
inline size_t word_count(size_t bits) { return bits / word_bits + (bits % word_bits != 0); }
inline Word low_mask(size_t bits) {
    return bits == 0 ? Word{0} : bits >= word_bits ? ~Word{0} : (Word{1} << bits) - 1;
}
inline Word word_mask(size_t index, size_t bits) {
    const size_t start = index * word_bits;
    return start >= bits ? Word{0} : low_mask(bits - start);
}
inline size_t popcount(Word value) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(GRAPHMINI_BIT_OPS_PORTABLE)
    return static_cast<size_t>(__builtin_popcountll(value));
#else
    // Portable fallback: no unconditional POPCNT ISA requirement on MSVC.
    value -= (value >> 1) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (value * UINT64_C(0x0101010101010101)) >> 56;
#endif
}
inline size_t trailing_zeros(Word value) {
    if (!value)
        return word_bits;
#if (defined(__GNUC__) || defined(__clang__)) && !defined(GRAPHMINI_BIT_OPS_PORTABLE)
    return static_cast<size_t>(__builtin_ctzll(value));
#else
    size_t count = 0;
    while (!(value & 1)) {
        value >>= 1;
        ++count;
    }
    return count;
#endif
}

// Buffers have exactly word_count(bits) naturally aligned words, with no SIMD
// padding requirement. Null is valid for zero bits. Exact output/input aliasing
// is supported; partial overlap is not. All reads ignore unused tail bits and
// writes clear them. limit is an exclusive LOCAL bit position, not a vertex ID.
enum class Binary { Intersection, Difference };
// Allow tests/benchmarks to retain compiler builtins while bypassing explicit
// vector kernels. PORTABLE additionally forces portable word primitives.
inline size_t simd_word_width() {
#if defined(GRAPHMINI_BIT_OPS_PORTABLE) || defined(GRAPHMINI_BIT_OPS_SCALAR)
    return 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return 2;
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
    return internal::has_avx2() ? 4 : 0;
#else
    return 0;
#endif
}
template <Binary Op, bool Write, bool Simd = true>
inline size_t combine(const Word *a, const Word *b, size_t bits, Word *out = nullptr,
                      size_t limit = unlimited) {
    const size_t active_bits = std::min(bits, limit);
    // Terminal counts often fit in one word. Bypass loop/vector dispatch
    // entirely, including when a bound narrows a larger universe to one word.
    if constexpr (!Write) {
        if (active_bits <= word_bits) {
            if (!active_bits)
                return 0;
            const Word value = Op == Binary::Intersection ? a[0] & b[0] : a[0] & ~b[0];
            return popcount(value & low_mask(active_bits));
        }
    }
    const size_t active_words = word_count(active_bits);
    size_t count = 0;
    size_t processed = 0;
    if constexpr (Simd) {
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr size_t minimum_bits = 128;
#else
        constexpr size_t minimum_bits = 256;
#endif
        // Avoid even the runtime x86 feature check for sub-vector inputs.
        if (active_bits >= minimum_bits) {
            const size_t width = simd_word_width();
            if (width) {
                processed = (active_bits / word_bits) / width * width;
                if (processed) {
#if defined(__aarch64__) || defined(_M_ARM64)
                    count = internal::neon_words<Op == Binary::Difference, Write>(a, b, processed, out);
#elif defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
                    count = internal::avx2_words<Op == Binary::Difference, Write>(a, b, processed, out);
#endif
                }
            }
        }
    }
    for (size_t i = processed; i < active_words; ++i) {
        Word value = (Op == Binary::Intersection ? a[i] & b[i] : a[i] & ~b[i]);
        value &= word_mask(i, active_bits);
        if constexpr (Write)
            out[i] = value;
        count += popcount(value);
    }
    if constexpr (Write)
        for (size_t i = active_words; i < word_count(bits); ++i)
            out[i] = 0;
    return count;
}
inline size_t intersection_count(const Word *a, const Word *b, size_t bits, size_t limit = unlimited) {
    return combine<Binary::Intersection, false>(a, b, bits, nullptr, limit);
}
// Words=0 retains dynamic SIMD dispatch. Fixed callers must establish
// word_count(bits)==Words once at region entry; buffers need no extra padding.
template<size_t Words, Binary Op, bool Write>
inline size_t combine_fixed(const Word *a, const Word *b, size_t bits, Word *out = nullptr,
                            size_t limit = unlimited) {
    static_assert(Words <= 2, "Only one/two-word specializations are supported");
    if constexpr (Words == 0) {
        return combine<Op, Write>(a, b, bits, out, limit);
    } else {
        const size_t active = std::min(bits, limit);
        size_t result = 0;
        for (size_t i = 0; i < Words; ++i) {
            Word value = Op == Binary::Intersection ? a[i] & b[i] : a[i] & ~b[i];
            value &= word_mask(i, active);
            result += popcount(value);
            if constexpr (Write) out[i] = value;
        }
        return result;
    }
}
inline size_t difference_count(const Word *a, const Word *b, size_t bits, size_t limit = unlimited) {
    return combine<Binary::Difference, false>(a, b, bits, nullptr, limit);
}
inline size_t intersection_write(const Word *a, const Word *b, size_t bits, Word *out,
                                 size_t limit = unlimited) {
    return combine<Binary::Intersection, true>(a, b, bits, out, limit);
}
inline size_t difference_write(const Word *a, const Word *b, size_t bits, Word *out,
                               size_t limit = unlimited) {
    return combine<Binary::Difference, true>(a, b, bits, out, limit);
}
inline size_t count(const Word *a, size_t bits, size_t limit = unlimited) {
    return intersection_count(a, a, bits, limit);
}
inline void copy_prefix(const Word *a, size_t bits, Word *out, size_t limit = unlimited) {
    const size_t active = std::min(bits, limit);
    for (size_t i = 0; i < word_count(bits); ++i)
        out[i] = a[i] & word_mask(i, active);
}
inline bool test(const Word *a, size_t bits, size_t position) {
    return position < bits && (a[position / word_bits] & (Word{1} << (position % word_bits)));
}
inline void clear(Word *a, size_t bits, size_t position) {
    if (position < bits)
        a[position / word_bits] &= ~(Word{1} << (position % word_bits));
}
template <class Visitor> inline void for_each(const Word *a, size_t bits, Visitor visit) {
    for (size_t i = 0; i < word_count(bits); ++i) {
        Word value = a[i] & word_mask(i, bits);
        while (value) {
            visit(i * word_bits + trailing_zeros(value));
            value &= value - 1;
        }
    }
}
} // namespace minigraph::bit_ops
