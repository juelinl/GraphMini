#pragma once
#include "avx2_decode.h"
#include "bit_ops.h"
#include <stdexcept>

namespace minigraph::bit_ops {
inline bool has_avx2_decoder() {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__)) &&                                 \
    !defined(GRAPHMINI_BIT_OPS_SCALAR) && !defined(GRAPHMINI_BIT_OPS_PORTABLE)
    return internal::has_avx2();
#else
    return false;
#endif
}
// Experimental bulk decoding into ascending LOCAL indices, not global IDs.
// bits <= UINT32_MAX; input contains exactly word_count(bits) words; unused
// high tail bits are ignored. Output capacity must be >= count(a,bits), with
// no padding required. Input/output may not overlap. Null buffers are allowed
// when they would not be accessed (zero bits / zero set bits respectively).
inline size_t decode_indices_scalar(const Word *a, size_t bits, uint32_t *out) {
    if (bits > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Bitmap local indices exceed uint32_t universe size");
    size_t written = 0;
    for_each(a, bits, [&](size_t position) { out[written++] = static_cast<uint32_t>(position); });
    return written;
}
// Explicit experiment, not an automatic density-selection policy. Falls back
// safely on machines without AVX2; callers can query has_avx2_decoder().
inline size_t decode_indices_avx2(const Word *a, size_t bits, uint32_t *out) {
    if (bits > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Bitmap local indices exceed uint32_t universe size");
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
    if (has_avx2_decoder())
        return internal::avx2_decode_indices(a, bits, out);
#endif
    return decode_indices_scalar(a, bits, out);
}
} // namespace minigraph::bit_ops
