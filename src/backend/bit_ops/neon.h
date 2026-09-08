#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
namespace minigraph::bit_ops::internal {
// Full pairs of uint64 words only. No alignment or padding beyond uint64_t.
template <bool Subtract, bool Write>
inline size_t neon_words(const uint64_t *a, const uint64_t *b, size_t words, uint64_t *out) {
    auto totals = vdupq_n_u64(0);
    for (size_t i = 0; i < words; i += 2) {
        const auto left = vld1q_u64(a + i);
        const auto right = vld1q_u64(b + i);
        const auto value = Subtract ? vbicq_u64(left, right) : vandq_u64(left, right);
        if constexpr (Write)
            vst1q_u64(out + i, value);
        const auto bytes = vcntq_u8(vreinterpretq_u8_u64(value));
        totals = vaddq_u64(totals, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(bytes))));
    }
    return static_cast<size_t>(vaddvq_u64(totals));
}
} // namespace minigraph::bit_ops::internal
#endif
