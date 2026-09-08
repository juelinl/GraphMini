#include "backend/bit_ops/decode.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace ops = minigraph::bit_ops;
#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif
NOINLINE size_t scalar(const uint64_t *a, size_t bits, uint32_t *out) {
    return ops::decode_indices_scalar(a, bits, out);
}
NOINLINE size_t dispatched(const uint64_t *a, size_t bits, uint32_t *out) {
    return ops::decode_indices_avx2(a, bits, out);
}
using Decoder = size_t (*)(const uint64_t *, size_t, uint32_t *);
volatile uint64_t observed = 0;
double measure(Decoder decoder, const std::vector<std::vector<uint64_t>> &inputs, size_t bits,
               size_t repeats, std::vector<uint32_t> &out) {
    uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < repeats; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto count = decoder(inputs[i % inputs.size()].data(), bits, out.data());
        checksum += count;
        if (count)
            checksum += out[i % count];
    }
    const auto end = std::chrono::steady_clock::now();
    observed = checksum;
    return std::chrono::duration<double, std::nano>(end - start).count() / repeats;
}
int main() {
    std::cout << "# AVX2=" << ops::has_avx2_decoder() << '\n';
    if (!ops::has_avx2_decoder()) {
        std::cout << "# No AVX2 decoder: benchmark skipped, regression tests cover scalar fallback.\n";
        return 0;
    }
    std::mt19937_64 rng(20260909);
    std::cout << "bits,density_percent,mean_set_bits,trial,scalar_ns,avx2_ns\n";
    for (size_t bits : {64, 256, 4096, 65536})
        for (unsigned density : {0, 1, 2, 5, 10, 25, 50, 75, 100}) {
            std::vector<std::vector<uint64_t>> inputs(32, std::vector<uint64_t>(ops::word_count(bits)));
            size_t total = 0;
            std::vector<uint32_t> a(bits), b(bits);
            for (auto &words : inputs) {
                for (size_t i = 0; i < bits; ++i)
                    if (rng() % 100 < density)
                        words[i / 64] |= uint64_t{1} << (i % 64);
                const size_t count = scalar(words.data(), bits, a.data());
                if (dispatched(words.data(), bits, b.data()) != count ||
                    !std::equal(a.begin(), a.begin() + count, b.begin()))
                    throw std::logic_error("Benchmark decoder mismatch");
                total += count;
            }
            const size_t repeats =
                std::clamp<size_t>(1000000 / (ops::word_count(bits) + total / 32 + 1), 200, 50000);
            for (int trial = 0; trial < 7; ++trial) {
                double s, v;
                if (trial % 2) {
                    v = measure(dispatched, inputs, bits, repeats, b);
                    s = measure(scalar, inputs, bits, repeats, a);
                } else {
                    s = measure(scalar, inputs, bits, repeats, a);
                    v = measure(dispatched, inputs, bits, repeats, b);
                }
                std::cout << bits << ',' << density << ',' << double(total) / 32 << ',' << trial << ','
                          << s << ',' << v << '\n';
            }
        }
}
