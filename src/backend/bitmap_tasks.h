#pragma once
#include "bitmap_count_region.h"
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <functional>

namespace minigraph {
// No mutable state is stored in a TBB body: each invocation has its own slots
// and reduction accumulator. Parent state stays read-only until the join.
template<class Function>
uint64_t bitmap_for_each(BitmapCountRegion &state, size_t input, bool parallel,
                         const Function &function) {
    auto serial = [&](BitmapCountRegion &local, size_t begin, size_t end) {
        uint64_t count = 0;
        for (auto cursor = local.local_cursor(input, begin, end); cursor.valid(); cursor.advance())
            count += function(local, cursor.position());
        return count;
    };
    if (!parallel || state.input_size(input) < 2)
        return serial(state, 0, state.universe_size());
    return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, state.universe_size(), 16), uint64_t{0},
        [&](const tbb::blocked_range<size_t> &range, uint64_t count) {
            auto local = state.fork();
            return count + serial(local, range.begin(), range.end());
        }, std::plus<uint64_t>{});
}
} // namespace minigraph
