#pragma once
#include "bitgraph.h"
#include <memory>

namespace minigraph {
// Initial terminal-count specialization. Own everything borrowed by the final
// loop; the original array prefixes remain available if the budget rejects it.
class BitmapCountRegion {
    BitGraph graph_;
    std::vector<Bitmap> inputs_;

    template <class Graph, class Set>
    BitmapCountRegion(const Graph &graph, uint32_t anchor, const Set &neighbors,
                      const Set &rows, const std::vector<const Set *> &inputs)
        : graph_(NeighborhoodUniverse(anchor, neighbors.data(), neighbors.size()),
                 std::vector<uint32_t>(rows.data(), rows.data() + rows.size()),
                 [&](uint32_t vertex) { return graph.N(vertex); }) {
        inputs_.reserve(inputs.size());
        for (const auto *input : inputs)
            inputs_.push_back(Bitmap::from_sorted(graph_.universe(), input->data(), input->size()));
    }

  public:
    // Includes persistent row words, candidate words, ID mappings and object
    // storage plus one construction scratch bitmap. Allocator overhead is not
    // an exact resident-memory guarantee. Zero budget forces array fallback.
    template <class Graph, class Set>
    static std::unique_ptr<BitmapCountRegion> build(
        const Graph &graph, uint32_t anchor, const Set &neighbors, const Set &rows,
        const std::vector<const Set *> &inputs, size_t budget = 32 * 1024 * 1024) {
        if (!rows.size() || !neighbors.size())
            return {};
        auto charge = [&](size_t count, size_t width) {
            if (width && count > budget / width)
                return false;
            budget -= count * width;
            return true;
        };
        const size_t stride = bit_ops::word_count(neighbors.size()) * sizeof(bit_ops::Word);
        if (!charge(1, sizeof(BitmapCountRegion)) || !charge(neighbors.size(), sizeof(uint32_t)) ||
            !charge(rows.size(), sizeof(uint32_t)) || !charge(rows.size(), stride) ||
            !charge(inputs.size(), stride) || !charge(inputs.size(), sizeof(Bitmap)) ||
            !charge(1, stride))
            return {};
        return std::unique_ptr<BitmapCountRegion>(
            new BitmapCountRegion(graph, anchor, neighbors, rows, inputs));
    }
    size_t count(size_t input, uint32_t vertex, bool subtract,
                 std::optional<uint32_t> upper = {}) const {
        const auto source = inputs_.at(input).view();
        const auto row = graph_.row(vertex);
        return subtract ? source.subtract_count(row, vertex, upper)
                        : source.intersect_count(row, upper);
    }
    size_t row_count() const { return graph_.row_count(); }
};
} // namespace minigraph
