#include "backend/bit_ops/decode.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace ops = minigraph::bit_ops;
void check(const uint64_t *a, size_t bits) {
    std::vector<uint32_t> expected;
    // Independent bit-by-bit oracle (not CTZ or the decoding lookup table).
    for (size_t i = 0; i < bits; ++i)
        if ((a[i / 64] >> (i % 64)) & 1)
            expected.push_back(static_cast<uint32_t>(i));
    for (bool simd : {false, true}) {
        // Offset output and canaries check non-vector alignment and overstores.
        std::vector<uint32_t> buffer(expected.size() + 2, 0xfefefefe);
        auto decode = simd ? ops::decode_indices_avx2 : ops::decode_indices_scalar;
        auto count = decode(a, bits, buffer.data() + 1);
        if (count != expected.size() || buffer.front() != 0xfefefefe || buffer.back() != 0xfefefefe ||
            !std::equal(expected.begin(), expected.end(), buffer.begin() + 1))
            throw std::logic_error("Bitmap decoder mismatch/overstore");
        // Separate exact-sized allocation lets sanitizers catch padded stores.
        auto exact = std::make_unique<uint32_t[]>(expected.size());
        if (decode(a, bits, exact.get()) != count ||
            !std::equal(expected.begin(), expected.end(), exact.get()))
            throw std::logic_error("Exact-sized decode failed");
    }
}
int main() {
    size_t cases = 0;
    if (ops::decode_indices_avx2(nullptr, 0, nullptr) != 0)
        throw std::logic_error("Empty decode failed");
    // Every 16-bit mask tests byte packing and all adjacent-byte combinations.
    for (uint64_t value = 0; value < 65536; ++value) {
        check(&value, 16);
        ++cases;
    }
    std::mt19937_64 rng(20260909);
    for (size_t bits : {1,   7,   8,   9,   31,  32,  63,  64,   65,   127,  128,
                        129, 255, 256, 257, 511, 512, 513, 4095, 4096, 4097, 65537})
        for (int density = 0; density <= 64; ++density) {
            // Input begins one word into an allocation ending exactly at its tail.
            auto storage = std::make_unique<uint64_t[]>(ops::word_count(bits) + 1);
            auto *a = storage.get() + 1;
            for (size_t i = 0; i < ops::word_count(bits); ++i) {
                a[i] = 0;
                for (int j = 0; j < 64; ++j)
                    if (rng() % 64 < static_cast<unsigned>(density))
                        a[i] |= uint64_t{1} << j;
            }
            // Tail bits deliberately dirty.
            if (bits % 64)
                a[ops::word_count(bits) - 1] |= ~ops::low_mask(bits % 64);
            check(a, bits);
            ++cases;
        }
    uint64_t empty = 0;
    if (ops::decode_indices_avx2(&empty, 64, nullptr) != 0)
        throw std::logic_error("Zero-output decode failed");
    bool rejected = false;
    try {
        ops::decode_indices_avx2(nullptr, size_t{UINT32_MAX} + 1, nullptr);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        throw std::logic_error("Oversized universe accepted");
    std::cout << "Validated " << cases << " decoder cases; AVX2=" << ops::has_avx2_decoder() << '\n';
}
