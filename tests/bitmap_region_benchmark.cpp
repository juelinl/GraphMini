// Compile against both revisions; define GRAPHMINI_LOCAL_BITMAP for the new API.
#include "backend/bitmap_count_region.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>

using namespace minigraph;
struct Set {
    const uint32_t *ptr;
    size_t length;
    const uint32_t *data() const { return ptr; }
    size_t size() const { return length; }
};
struct Graph {
    std::vector<std::vector<uint32_t>> rows;
    Set N(uint32_t id) const { return {rows.at(id / 2).data(), rows.at(id / 2).size()}; }
};
template <class F> double measure(size_t repetitions, F work) {
    std::vector<double> trials;
    for (int trial = 0; trial < 7; ++trial) {
        uint64_t checksum = 0;
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < repetitions; ++i) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            checksum += work(i);
        }
        auto end = std::chrono::steady_clock::now();
        trials.push_back(std::chrono::duration<double, std::nano>(end - start).count() / repetitions);
        if (!checksum) throw std::runtime_error("Empty benchmark checksum");
    }
    std::sort(trials.begin(), trials.end());
    return trials[trials.size() / 2];
}
int main() {
    std::cout << "universe,density,build_ns,bind_two_inputs_ns,iterate_count_ns,checksum\n";
    for (size_t size : {64, 256, 1024}) {
        for (unsigned density : {10, 50, 90}) {
            std::mt19937_64 rng(20260908);
            Graph graph;
            graph.rows.resize(size);
            std::vector<uint32_t> ids(size), candidate, iterator;
            for (size_t i = 0; i < size; ++i) {
                ids[i] = 2 * i;
                if (rng() % 100 < density) candidate.push_back(ids[i]);
                if (rng() % 100 < density) iterator.push_back(ids[i]);
                for (size_t j = 0; j < i; ++j) {
                    if (rng() % 100 < density) {
                        graph.rows[i].push_back(2 * j);
                        graph.rows[j].push_back(2 * i);
                    }
                }
            }
            const Set domain{ids.data(), size}, input{candidate.data(), candidate.size()},
                      loop{iterator.data(), iterator.size()};
            auto build = [&] { return BitmapCountRegion::build(graph, UINT32_MAX, domain, domain, 2); };
            auto region = build();
            auto bind = [&] {
#ifdef GRAPHMINI_LOCAL_BITMAP
                const Set *inputs[] = {&input, &loop};
                region->bind_inputs(inputs, 2);
#else
                region->bind_inputs(std::vector<const Set *>{&input, &loop});
#endif
            };
            bind();
            uint64_t expected = 0;
            for (auto vertex : iterator)
                for (auto value : candidate)
                    if (value < vertex) {
                        const auto &row = graph.rows[vertex / 2];
                        bool found = std::binary_search(row.begin(), row.end(), value);
                        expected += (vertex / 2 % 2) ? !found : found;
                    }
            auto iterate = [&] {
                uint64_t count = 0;
#ifdef GRAPHMINI_LOCAL_BITMAP
                for (auto cursor = region->local_cursor(1); cursor.valid(); cursor.advance()) {
                    auto position = cursor.position();
                    count += region->count_local(0, position, position % 2, true);
                }
#else
                for (auto vertex : iterator)
                    count += region->count(0, vertex, vertex / 2 % 2, vertex);
#endif
                return count;
            };
            if (iterate() != expected) throw std::runtime_error("Independent count mismatch");
            // Consume a varying row so construction cannot become dead stores.
            // This phase includes one row count, but no live-in conversion.
            auto construction = measure(20, [&](size_t i) {
                BitGraph rows(NeighborhoodUniverse(UINT32_MAX, ids), ids,
                              [&](uint32_t vertex) { return graph.N(vertex); });
                auto row = (i * 37) % size;
                auto count = rows.row(ids[row]).count();
                if (count != graph.rows[row].size())
                    throw std::runtime_error("Constructed row mismatch");
                return count + 1;
            });
            auto conversion = measure(2000, [&](size_t) { bind(); return size; });
            auto counting = measure(1000, [&](size_t) { return iterate() + 1; });
            std::cout << size << ',' << density << ',' << construction << ',' << conversion << ','
                      << counting << ',' << expected << '\n';
        }
    }
}
