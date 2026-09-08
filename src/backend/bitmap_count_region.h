#pragma once
#include "bitgraph.h"
#include <memory>

namespace minigraph {
// Initial terminal-count specialization. Own everything borrowed by the final
// loop; the original array prefixes remain available if the budget rejects it.
class BitmapCountRegion {
    BitGraph graph_;
    std::vector<Bitmap> inputs_;
    size_t input_count_;

    template <class Graph, class Set>
    BitmapCountRegion(const Graph &graph, uint32_t anchor, const Set &neighbors, const Set &rows,
                      size_t input_count)
        : graph_(NeighborhoodUniverse(anchor, neighbors.data(), neighbors.size()),
                 std::vector<uint32_t>(rows.data(), rows.data() + rows.size()),
                 [&](uint32_t vertex) { return graph.N(vertex); }),
          input_count_(input_count) {
        inputs_.reserve(input_count);
    }

  public:
    // Includes persistent row words, candidate words, ID mappings and object
    // storage plus one construction scratch bitmap. Allocator overhead is not
    // an exact resident-memory guarantee. Zero budget forces array fallback.
    template <class Graph, class Set>
    static std::unique_ptr<BitmapCountRegion>
    build(const Graph &graph, uint32_t anchor, const Set &neighbors, const Set &rows, size_t input_count,
          size_t budget = 32 * 1024 * 1024) {
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
            !charge(input_count, stride) || !charge(input_count, sizeof(Bitmap)) || !charge(1, stride))
            return {};
        return std::unique_ptr<BitmapCountRegion>(
            new BitmapCountRegion(graph, anchor, neighbors, rows, input_count));
    }
    // Called once per penultimate prefix binding. Rows retain their original
    // universe and are reused across these bindings; no nested BitGraph.
    template <class Set> void bind_inputs(const std::vector<const Set *> &inputs) {
        if (inputs.size() != input_count_)
            throw std::invalid_argument("Bitmap region live-in count mismatch");
        inputs_.clear();
        for (const auto *input : inputs) {
            if (!input)
                throw std::invalid_argument("Null bitmap region live-in");
            inputs_.push_back(Bitmap::from_sorted(graph_.universe(), input->data(), input->size()));
        }
    }
    size_t count(size_t input, uint32_t vertex, bool subtract,
                 std::optional<uint32_t> upper = {}) const {
        const auto source = inputs_.at(input).view();
        const auto row = graph_.row(vertex);
        return subtract ? source.subtract_count(row, vertex, upper) : source.intersect_count(row, upper);
    }
    size_t row_count() const { return graph_.row_count(); }
};
} // namespace minigraph
