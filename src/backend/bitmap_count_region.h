#pragma once
#include <cstddef>
#include <cstdint>
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
        for (size_t i = 0; i < input_count; ++i)
            inputs_.emplace_back(graph_.universe());
    }

  public:
    // Borrowed until region destruction/rebinding. Construct after binding and
    // consume within that prefix's loop; never cache across bindings.
    class CountingView {
        const bit_ops::Word *source_, *rows_;
        size_t bits_, stride_;
        friend class BitmapCountRegion;
        CountingView(const bit_ops::Word *source, const bit_ops::Word *rows, size_t bits)
            : source_(source), rows_(rows), bits_(bits), stride_(bit_ops::word_count(bits)) {}
      public:
        size_t count(uint32_t position, bool subtract, bool bounded) const {
            if (position >= bits_) throw std::out_of_range("Invalid bitmap row position");
            const auto *row = rows_ + position * stride_;
            const size_t limit = bounded ? position : bits_;
            if (!subtract) return bit_ops::intersection_count(source_, row, bits_, limit);
            size_t result = bit_ops::difference_count(source_, row, bits_, limit);
            if (position < limit && bit_ops::test(source_, bits_, position) &&
                !bit_ops::test(row, bits_, position)) --result;
            return result;
        }
    };
    CountingView counting_view(size_t input) const & {
        if (!graph_.has_universe_rows())
            throw std::logic_error("Local counting requires universe-indexed rows");
        return {inputs_.at(input).words().data(), graph_.row_data_at(0), graph_.universe().size()};
    }
    CountingView counting_view(size_t) const && = delete;
    template<class Set> void bind_input(size_t index, const Set &input) {
        inputs_.at(index).assign_sorted(input.data(), input.size());
    }
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
    template <class Set> void bind_inputs(const Set *const *inputs, size_t size) {
        if (size != input_count_ || (size && !inputs))
            throw std::invalid_argument("Bitmap region live-in count mismatch");
        for (size_t i = 0; i < size; ++i)
            if (!inputs[i])
                throw std::invalid_argument("Null bitmap region live-in");
        for (size_t i = 0; i < size; ++i)
            inputs_[i].assign_sorted(inputs[i]->data(), inputs[i]->size());
    }
    template <class Set> void bind_inputs(const std::vector<const Set *> &inputs) {
        bind_inputs(inputs.data(), inputs.size());
    }
    BitmapView input_view(size_t input) const & { return inputs_.at(input).view(); }
    BitmapView input_view(size_t) const && = delete;
    BitmapLocalCursor local_cursor(size_t input) const & {
        if (!graph_.has_universe_rows())
            throw std::logic_error("Local iteration requires universe-indexed rows");
        return {inputs_.at(input).words().data(), graph_.universe().size()};
    }
    BitmapLocalCursor local_cursor(size_t) const && = delete;
    // Canonicality against the selected local vertex is order-preserving because
    // universe IDs are sorted. Exclusion also stays entirely in local coordinates.
    size_t count_local(size_t input, uint32_t position, bool subtract, bool bounded = false) const {
        if (!graph_.has_universe_rows())
            throw std::logic_error("Local counting requires universe-indexed rows");
        const auto *source = inputs_.at(input).words().data();
        const auto *row = graph_.row_data_at(position);
        const auto bits = graph_.universe().size();
        const size_t limit = bounded ? position : bits;
        if (!subtract)
            return bit_ops::intersection_count(source, row, bits, limit);
        size_t count = bit_ops::difference_count(source, row, bits, limit);
        if (position < limit && bit_ops::test(source, bits, position) &&
            !bit_ops::test(row, bits, position))
            --count;
        return count;
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
