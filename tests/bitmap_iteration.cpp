#include "backend/bitmap_tasks.h"
#include <oneapi/tbb/global_control.h>
#include <atomic>
#include <iostream>
#include <numeric>
#include <random>
#include <type_traits>

using namespace minigraph;
void require(bool ok) { if (!ok) throw std::runtime_error("Bitmap iteration regression"); }
int main() {
    std::mt19937 rng(20260910);
    size_t cases = 0;
    for (int threads : {1, 4}) {
        tbb::global_control workers(tbb::global_control::max_allowed_parallelism, threads);
        for (size_t n : {0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1025, 4097}) {
            std::vector<uint32_t> ids(n);
            for (size_t i = 0; i < n; ++i) ids[i] = 3 * i + 19;
            NeighborhoodUniverse universe(7, ids);
            for (int density : {0, 1, 10, 50, 100}) {
                Bitmap input(universe);
                std::vector<bool> selected(n);
                uint64_t expected = 0;
                for (size_t i = 0; i < n; ++i) {
                    if ((selected[i] = rng() % 100 < static_cast<unsigned>(density))) {
                        input.set(ids[i]);
                        expected += i + 1;
                    }
                }
                const auto before = input.view().vertices();
                for (auto mode : {BitmapIteration::Positions, BitmapIteration::DecodedScalar, BitmapIteration::DecodedAVX2})
                for (size_t grain : {1, 64})
                for (bool parallel : {false, true}) {
                    auto seen = std::make_unique<std::atomic_uint[]>(n);
                    for (size_t i = 0; i < n; ++i) seen[i] = 0;
                    BitmapTaskPolicy policy{grain, 99, true, false, mode};
                    const auto total = bitmap_for_each(input, parallel, [&](auto cursor, bool task) -> uint64_t {
                        constexpr bool decoded = std::is_same_v<decltype(cursor), BitmapIndexCursor>;
                        require(decoded == (parallel && input.count() >= 2 && mode != BitmapIteration::Positions));
                        require(task == (parallel && input.count() >= 2));
                        // skip_empty in position mode and candidate slicing in decoded mode
                        // must never invoke a body for an empty slice.
                        require(cursor.valid());
                        uint64_t sum = 0;
                        uint32_t previous = 0;
                        bool first = true;
                        for (; cursor.valid(); cursor.advance()) {
                            const auto idx = cursor.position();
                            require(idx < n && selected[idx] && (first || idx > previous));
                            seen[idx].fetch_add(1);
                            sum += idx + 1;
                            previous = idx;
                            first = false;
                        }
                        return sum;
                    }, policy);
                    require(total == expected && input.view().vertices() == before);
                    for (size_t i = 0; i < n; ++i) require(seen[i] == static_cast<unsigned>(selected[i]));
                    ++cases;
                }
            }
        }
        NeighborhoodUniverse universe(7, {19, 22, 25, 28});
        Bitmap full(universe, true);
        for (auto mode : {BitmapIteration::DecodedScalar, BitmapIteration::DecodedAVX2}) {
            BitmapTaskPolicy policy{1, 99, true, false, mode};
            bool caught = false;
            try {
                bitmap_for_each(full, true, [](auto, bool) -> uint64_t { throw std::runtime_error("cancel"); }, policy);
            } catch (const std::runtime_error &) { caught = true; }
            require(caught);
            policy.levels = 0;
            require(bitmap_for_each(full, true, [](auto cursor, bool task) -> uint64_t {
                require(!task && (std::is_same_v<decltype(cursor), BitmapLocalCursor>));
                uint64_t count = 0;
                for (; cursor.valid(); cursor.advance()) ++count;
                return count;
            }, policy) == 4);
        }
    }
    std::cout << "Validated " << cases << " cursor/slicing cases; AVX2=" << bit_ops::has_avx2_decoder() << '\n';
}
