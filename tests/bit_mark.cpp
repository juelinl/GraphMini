#include "backend/bit_ops/from_sorted.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace minigraph;
void check(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    const size_t nw = bit_ops::word_count(a.size());
    std::vector<uint64_t> expected(nw), actual(nw + 2, ~uint64_t{0});
    size_t count = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::binary_search(b.begin(), b.end(), a[i])) {
            expected[i / 64] |= uint64_t{1} << (i % 64);
            ++count;
        }
    const auto got = bit_ops::from_sorted(a.data(), a.size(), b.data(), b.size(), actual.data() + 1);
    if (got != count || !std::equal(expected.begin(), expected.end(), actual.begin() + 1) ||
        actual.front() != ~uint64_t{0} || actual.back() != ~uint64_t{0})
        throw std::runtime_error("mark kernel disagrees with membership oracle");
    std::vector<uint64_t> scalar(nw);
    if (bit_ops::from_sorted<false>(a.data(), a.size(), b.data(), b.size(), scalar.data()) != count ||
        scalar != expected)
        throw std::runtime_error("scalar mark mismatch");
}
int main(int argc, char **) {
    std::mt19937 rng(20260908);
    check({}, {});
    check({0, 1, 0x7fffffffu, 0x80000000u, UINT32_MAX}, {0, 0x80000000u, UINT32_MAX});
    for (size_t n = 0; n <= 140; ++n)
        for (size_t m = 0; m <= 140; ++m) {
            std::vector<uint32_t> a(n), b(m);
            uint32_t x = 0x7ffffe00u, y = x;
            for (auto &v : a) v = (x += 1 + rng() % 9);
            for (auto &v : b) v = (y += 1 + rng() % 9);
            check(a, b);
        }
    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<uint32_t> a, b;
        for (uint32_t i = 0; i < 4096; ++i) {
            if (rng() % 100 < unsigned(trial % 100)) a.push_back(UINT32_MAX - 4096 + i);
            if (rng() % 100 < unsigned((trial * 13) % 100)) b.push_back(UINT32_MAX - 4096 + i);
        }
        check(a, b); check(a, a);
    }
    std::cout << "mark membership checks passed; SIMD word width=" << bit_ops::simd_word_width() << '\n';
    if (argc == 1) return 0;
    volatile size_t sink = 0;
    for (size_t n : {64, 256, 1024})
        for (unsigned density : {10, 50, 90}) {
            std::vector<uint32_t> a(n), b;
            for (size_t i = 0; i < n; ++i) a[i] = uint32_t(i * 2);
            for (size_t i = 0; i < n * 2; ++i)
                if (rng() % 100 < density) b.push_back(uint32_t(i));
            std::vector<uint64_t> out(bit_ops::word_count(n));
            std::vector<double> scalar, simd;
            for (int trial = 0; trial < 8; ++trial)
                for (int order = 0; order < 2; ++order) {
                    const bool vector = (trial + order) % 2;
                    const auto start = std::chrono::steady_clock::now();
                    for (int r = 0; r < 20000; ++r) {
                        std::atomic_signal_fence(std::memory_order_seq_cst);
                        sink += vector ? bit_ops::from_sorted(a.data(), n, b.data(), b.size(), out.data())
                                       : bit_ops::from_sorted<false>(a.data(), n, b.data(), b.size(), out.data());
                        sink += out[size_t(r) % out.size()];
                    }
                    const double ns = std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - start).count() / 20000;
                    (vector ? simd : scalar).push_back(ns);
                }
            std::sort(scalar.begin(), scalar.end()); std::sort(simd.begin(), simd.end());
            std::cout << n << ',' << density << ',' << scalar[4] << ',' << simd[4] << ',' << scalar[4]/simd[4] << '\n';
        }
    return 0;
}
