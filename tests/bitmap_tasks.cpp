#include "backend/bitmap_tasks.h"
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>
#include <atomic>
#include <numeric>
#include <stdexcept>

int main() {
    using namespace minigraph;
    auto require = [](bool value) { if (!value) throw std::logic_error("Bitmap task isolation/range failure"); };
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism, 4);
    for (size_t n : {1, 63, 64, 65, 127, 128, 129, 257}) {
        struct Graph {
            std::vector<uint32_t> ids;
            mutable std::atomic<size_t> reads{0};
            const auto &N(uint32_t) const { ++reads; return ids; }
        } graph;
        graph.ids.resize(n);
        std::iota(graph.ids.begin(), graph.ids.end(), 10);
        auto rows = BitmapCountRegion::build_rows(graph, 9, graph.ids, graph.ids, 2);
        auto region = BitmapCountRegion::from_rows(rows, 2);
        // Independent prefix entries share rows, never candidate words. Keep a
        // state alive after releasing the original row-store handle as well.
        tbb::parallel_for(size_t{0}, size_t{32}, [&](size_t prefix) {
            auto state = BitmapCountRegion::from_rows(rows, 2);
            const std::vector<uint32_t> input(graph.ids.begin(), graph.ids.begin() + prefix % (n + 1));
            state->bind_input(0, input);
            require(state->input_size(0) == input.size());
            require(state->input_view(0).universe().compatible(region->input_view(0).universe()));
            require(state->input_view(0).data() != region->input_view(0).data());
            require(region->input_size(0) == 0);
        });
        require(graph.reads == n);
        rows.reset();
        require(!BitmapCountRegion::from_rows({}, 2));
        require(!BitmapCountRegion::build_rows(graph, 9, graph.ids, graph.ids, 2, 0));
        require(graph.reads == n); // Budget rejection does not read any rows.
        region->bind_input(0, graph.ids);
        region->bind_input(1, graph.ids);
        auto copy = region->fork();
        require(copy.input_view(0).universe().compatible(region->input_view(0).universe()));
        require(copy.input_view(0).data() != region->input_view(0).data());
        copy.bind_input(0, std::vector<uint32_t>{});
        require(region->input_size(0) == n && copy.input_size(0) == 0);
        for (size_t begin = 0; begin <= n; ++begin) {
            for (size_t end = begin; end <= n; ++end) {
                size_t seen = begin;
                for (auto cursor = region->local_cursor(0, begin, end); cursor.valid(); cursor.advance())
                    require(cursor.position() == seen++);
                require(seen == end);
            }
        }
        for (const BitmapTaskPolicy policy : {BitmapTaskPolicy{}, BitmapTaskPolicy{16, 99, true},
              BitmapTaskPolicy{64, 99, true}, BitmapTaskPolicy{128, 99, true}, BitmapTaskPolicy{64, 1, true}}) {
          for (int repeat = 0; repeat < 4; ++repeat) {
            const auto result = bitmap_for_each(*region, 0, true,
                [&](BitmapCountRegion &local, uint32_t position) {
                    // Produce [0,position), then recursively split its iteration.
                    local.materialize_local(1, 0, position, false, true);
                    return bitmap_for_each(local, 1, true,
                        [&](BitmapCountRegion &child, uint32_t inner) -> uint64_t {
                            require(child.input_size(1) == position);
                            return inner + 1;
                        }, policy, 1);
                }, policy, 0);
            require(result == n * (n-1) * (n+1) / 6);
            require(region->input_size(0) == n && region->input_size(1) == (n == 1 ? 0 : n));
            require(graph.reads == n); // No row rebuild in any nested task.
          }
          // Gaps and empty task ranges: only the two endpoints are candidates.
          std::vector<uint32_t> sparse{graph.ids.front()};
          if (n > 1) sparse.push_back(graph.ids.back());
          region->bind_input(0, sparse);
          require(bitmap_for_each(*region, 0, true,
              [](BitmapCountRegion &, uint32_t position) -> uint64_t { return position+1; }, policy)
              == (n == 1 ? 1 : n+1));
          region->bind_input(0, graph.ids);
        }
    }
}
