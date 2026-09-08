#include "backend/set_ops/set_ops.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using namespace minigraph;
volatile size_t sink = 0;
using Fn = size_t (*)(const uint32_t*, size_t, const uint32_t*, size_t, uint32_t*);
template<bool Write>
size_t baseline(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) {
    return set_ops::scalar<Write>(a, na, b, nb, out);
}
template<bool Write, bool Scalar>
size_t subtract(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) {
    if constexpr (Scalar) return set_ops::difference_scalar<Write>(a, na, b, nb, UINT32_MAX, out);
    else return set_ops::difference<Write>(a, na, b, nb, UINT32_MAX, out);
}
double measure(Fn fn, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out(a.size());
    const size_t iterations = std::max<size_t>(1000, 2000000 / (a.size() + b.size()));
    std::vector<double> samples;
    for (int trial = 0; trial < 5; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        size_t total = 0;
        for (size_t r = 0; r < iterations; ++r) {
            // Force each call to observe memory rather than hoisting a pure count.
            asm volatile("" ::: "memory");
            total += fn(a.data(), a.size(), b.data(), b.size(), out.data());
        }
        sink = total;
        samples.push_back(std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count() / iterations);
    }
    std::sort(samples.begin(), samples.end());
    return samples[2];
}
int main() {
    std::cout << "size,ratio,overlap,operation,scalar_ns,dispatch_ns\n";
    for (size_t n : {8, 16, 32, 64, 256, 1024})
        for (size_t ratio : {1, 16})
            for (bool overlap : {false, true}) {
                std::vector<uint32_t> a(n), b(n * ratio);
                for (size_t i = 0; i < a.size(); ++i) a[i] = i * 2 * ratio;
                for (size_t i = 0; i < b.size(); ++i) b[i] = i * 2 + !overlap;
                for (bool difference : {false, true}) for (bool write : {false, true}) {
                    const auto scalar = difference ? (write ? subtract<true, true> : subtract<false, true>)
                                                   : (write ? baseline<true> : baseline<false>);
                    const auto dispatch = difference ? (write ? subtract<true, false> : subtract<false, false>)
                                                     : (write ? set_ops::intersection<true> : set_ops::intersection<false>);
                    const double t0 = measure(scalar, a, b);
                    const double t1 = measure(dispatch, a, b);
                    std::cout << n << ',' << ratio << ',' << overlap << ',' <<
                        (difference ? "difference_" : "") << (write ? "write" : "count")
                        << ',' << t0 << ',' << t1 << '\n';
                }
            }
}
