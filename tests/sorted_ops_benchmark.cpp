#include "backend/set_ops/sorted.h"
#include <chrono>
#include <iostream>
#include <vector>
using namespace minigraph::set_ops;
volatile size_t sink;
template<class F> double measure(F f) {
    std::vector<double> times;
    for (int t = 0; t < 5; ++t) {
        const auto start = std::chrono::steady_clock::now();
        size_t sum = 0;
        for (size_t i = 0; i < 10000; ++i) { asm volatile("" ::: "memory"); sum += f(i); }
        sink = sum;
        times.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 10000);
    }
    std::sort(times.begin(), times.end()); return times[2];
}
int main() {
    std::cout << "operation,n,shape,scalar_ns,simd_ns,dispatch_ns\n";
    for (size_t n : {4, 8, 16, 32, 64, 128, 1024}) {
        std::vector<uint32_t> a(n), b(n), out(n);
        for (size_t i = 0; i < n; ++i) a[i] = 2 * i;
        for (int shape = 0; shape < 4; ++shape) {
            auto value = [&](size_t i) { return static_cast<uint32_t>(shape == 0 ? 0 : shape == 1 ? n : shape == 2 ? 2 * n : i % (2 * n + 1)); };
            std::cout << "search," << n << ',' << shape << ','
                << measure([&](size_t i) { return n <= 64 ? lower_bound_linear(a.data(), n, value(i)) : lower_bound_binary(a.data(), n, value(i)); }) << ','
                << (sorted_simd_available() ? measure([&](size_t i) { return lower_bound_simd(a.data(), n, value(i)); }) : 0) << ','
                << measure([&](size_t i) { return lower_bound_index(a.data(), n, value(i)); }) << '\n';
        }
        for (int shape = 0; shape < 3; ++shape) {
            for (size_t i = 0; i < n; ++i) b[i] = 2 * i + (shape == 0 ? 1 : shape == 1 ? 0 : i % 2);
            std::cout << "indices," << n << ',' << shape << ','
                << measure([&](size_t) { return indices_scalar(a.data(), n, b.data(), n, out.data()); }) << ','
                << (sorted_simd_available() ? measure([&](size_t) { return indices_simd(a.data(), n, b.data(), n, out.data()); }) : 0) << ','
                << measure([&](size_t) { return indices_write(a.data(), n, b.data(), n, out.data()); }) << '\n';
        }
    }
}
