#include "backend/bitmap.h"
#include "backend/set_ops/set_ops.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>

using namespace minigraph;
using Clock = std::chrono::steady_clock;
namespace {
template <class F> double time_ns(size_t iterations, F work) {
    const auto start = Clock::now();
    uint64_t sum = 0;
    for (size_t i = 0; i < iterations; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        sum += work(i);
    }
    const auto stop = Clock::now();
    if (!sum)
        std::cerr << "zero checksum\n";
    return std::chrono::duration<double, std::nano>(stop - start).count() / iterations;
}
} // namespace
int main() {
    std::mt19937_64 rng(20260907);
    constexpr size_t batch = 16;
    std::cout
        << "universe_size,density,bitmap_bytes,conversion_ns_per_set,array_count_ns,bitmap_count_ns\n";
    for (size_t size : {64, 256, 1024, 4096}) {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < size; ++i)
            ids.push_back(i * 2);
        NeighborhoodUniverse universe(UINT32_MAX, ids);
        for (unsigned density : {10, 50, 90}) {
            std::vector<std::vector<uint32_t>> arrays(batch);
            std::vector<Bitmap> bitmaps;
            std::vector<BitmapView> views;
            for (auto &array : arrays)
                for (auto id : ids)
                    if (rng() % 100 < density)
                        array.push_back(id);
            for (const auto &array : arrays)
                bitmaps.push_back(Bitmap::from_sorted(universe, array.data(), array.size()));
            for (const auto &bitmap : bitmaps)
                views.push_back(bitmap.view());
            for (size_t i = 0; i < batch; ++i) {
                const auto &a = arrays[i], &b = arrays[(i + 1) % batch];
                if (views[i].intersect_count(views[(i + 1) % batch]) !=
                    set_ops::intersection_count(a.data(), a.size(), b.data(), b.size()))
                    throw std::runtime_error("Benchmark count mismatch");
            }
            const double conversion = time_ns(1000, [&](size_t i) {
                const auto &array = arrays[i % batch];
                auto converted = Bitmap::from_sorted(universe, array.data(), array.size());
                return converted.view().count();
            });
            const double array_time = time_ns(20000, [&](size_t i) {
                const auto &a = arrays[i % batch], &b = arrays[(i + 1) % batch];
                return set_ops::intersection_count(a.data(), a.size(), b.data(), b.size());
            });
            const double bitmap_time = time_ns(20000, [&](size_t i) {
                return views[i % batch].intersect_count(views[(i + 1) % batch]);
            });
            std::cout << size << ',' << density << ','
                      << bit_ops::word_count(size) * sizeof(bit_ops::Word) << ',' << conversion << ','
                      << array_time << ',' << bitmap_time << '\n';
        }
    }
}
