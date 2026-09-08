#include "backend/bit_ops/bit_ops.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace ops = minigraph::bit_ops;
template <ops::Binary Op, bool Write, bool Simd>
double measure(const std::vector<std::vector<uint64_t>> &inputs, size_t bits) {
    std::vector<uint64_t> output(ops::word_count(bits));
    uint64_t checksum = 0;
    const size_t repeats = std::max<size_t>(2000, 16000000 / std::max<size_t>(ops::word_count(bits), 1));
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < repeats; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        checksum += ops::combine<Op, Write, Simd>(inputs[i % 16].data(), inputs[(i + 1) % 16].data(),
                                                  bits, Write ? output.data() : nullptr);
        if constexpr (Write)
            checksum += output[i % output.size()]; // Make stores observable.
    }
    const auto end = std::chrono::steady_clock::now();
    std::cout << checksum << ',';
    return std::chrono::duration<double, std::nano>(end - start).count() / repeats;
}
template <ops::Binary Op, bool Write>
void compare(const std::vector<std::vector<uint64_t>> &inputs, size_t bits) {
    std::vector<uint64_t> a(ops::word_count(bits)), b(a.size());
    auto scalar = ops::combine<Op, Write, false>(inputs[0].data(), inputs[1].data(), bits, a.data());
    auto simd = ops::combine<Op, Write, true>(inputs[0].data(), inputs[1].data(), bits, b.data());
    if (scalar != simd || (Write && a != b))
        throw std::runtime_error("Kernel benchmark mismatch");
    for (int trial = 0; trial < 7; ++trial) {
        std::cout << bits << ',' << (Op == ops::Binary::Intersection ? "and" : "andnot") << ',' << Write
                  << ',' << trial << ',';
        double scalar_ns, simd_ns;
        if (trial % 2 == 0) {
            scalar_ns = measure<Op, Write, false>(inputs, bits);
            simd_ns = measure<Op, Write, true>(inputs, bits);
        } else {
            simd_ns = measure<Op, Write, true>(inputs, bits);
            scalar_ns = measure<Op, Write, false>(inputs, bits);
        }
        std::cout << scalar_ns << ',' << simd_ns << '\n';
    }
}
int main() {
    std::mt19937_64 rng(20260908);
    std::cout << "# SIMD word width=" << ops::simd_word_width() << '\n';
    std::cout << "bits,op,write,trial,checksum1,checksum2,scalar_ns,simd_ns\n";
    for (size_t bits : {64, 128, 256, 512, 1024, 4096, 16384, 65536}) {
        std::vector<std::vector<uint64_t>> inputs(16, std::vector<uint64_t>(ops::word_count(bits)));
        for (auto &words : inputs)
            for (auto &word : words)
                word = rng();
        compare<ops::Binary::Intersection, false>(inputs, bits);
        compare<ops::Binary::Difference, false>(inputs, bits);
        compare<ops::Binary::Intersection, true>(inputs, bits);
        compare<ops::Binary::Difference, true>(inputs, bits);
    }
}
