#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
namespace minigraph::bit_ops::internal {
struct DecodeTable {
    std::array<std::array<uint8_t, 8>, 256> positions{};
    std::array<uint8_t, 256> counts{};
    constexpr DecodeTable() {
        for (unsigned mask = 0; mask < 256; ++mask)
            for (unsigned bit = 0; bit < 8; ++bit)
                if (mask & (1u << bit))
                    positions[mask][counts[mask]++] = static_cast<uint8_t>(bit);
    }
};
inline constexpr DecodeTable decode_table{};

// A byte selects up to eight ordered bit positions. Widen/add eight positions
// at once, then store exactly the live lanes: callers need no output padding.
__attribute__((target("avx2"))) inline size_t avx2_decode_indices(const uint64_t *a, size_t bits,
                                                                  uint32_t *out) {
    const auto lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    size_t written = 0;
    const size_t words = bits / 64 + (bits % 64 != 0);
    for (size_t i = 0; i < words; ++i) {
        uint64_t value = a[i];
        if (i + 1 == words && bits % 64)
            value &= (uint64_t{1} << (bits % 64)) - 1;
        for (unsigned byte = 0; value; ++byte, value >>= 8) {
            const auto mask = static_cast<uint8_t>(value);
            if (!mask)
                continue;
            const int count = decode_table.counts[mask];
            const auto packed =
                _mm_loadl_epi64(reinterpret_cast<const __m128i *>(decode_table.positions[mask].data()));
            const auto positions = _mm256_add_epi32(
                _mm256_cvtepu8_epi32(packed), _mm256_set1_epi32(static_cast<int>(i * 64 + byte * 8)));
            if (count == 8)
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + written), positions);
            else
                _mm256_maskstore_epi32(reinterpret_cast<int *>(out + written),
                                       _mm256_cmpgt_epi32(_mm256_set1_epi32(count), lanes), positions);
            written += count;
        }
    }
    return written;
}
} // namespace minigraph::bit_ops::internal
#endif
