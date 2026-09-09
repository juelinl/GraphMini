#pragma once
#include <cstddef>
#include <cstdint>
#include "bitgraph.h"
#include <memory>
#include <type_traits>
#include <utility>

namespace minigraph {
// Own rows and reusable candidate slots for terminal-only or full-region bitmap
// execution. Boundary arrays remain available if the budget rejects the region.
class BitmapCountRegion {
    std::shared_ptr<const BitGraph> graph_;
    std::vector<Bitmap> inputs_;
    size_t input_count_;

    BitmapCountRegion(std::shared_ptr<const BitGraph> graph, size_t input_count)
        : graph_(std::move(graph)), input_count_(input_count) {
        inputs_.reserve(input_count);
        for (size_t i = 0; i < input_count; ++i)
            inputs_.emplace_back(graph_->universe());
    }

  public:
    // Copy candidate words/cardinalities; retain exactly the same immutable rows
    // and universe identity. A synchronous nested task owns its copy exclusively.
    BitmapCountRegion fork() const { return *this; }
    size_t input_size(size_t input) const { return inputs_.at(input).count(); }
    BitmapLocalCursor local_cursor(size_t input, size_t begin, size_t end) const & {
        if (!graph_->has_universe_rows()) throw std::logic_error("Requires universe rows");
        return {inputs_.at(input).words().data(), graph_->universe().size(), begin, end};
    }
    BitmapLocalCursor local_cursor(size_t, size_t, size_t) const && = delete;
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
        if (!graph_->has_universe_rows())
            throw std::logic_error("Local counting requires universe-indexed rows");
        return {inputs_.at(input).words().data(), graph_->row_data_at(0), graph_->universe().size()};
    }
    CountingView counting_view(size_t) const && = delete;
    template<class Set> void bind_input(size_t index, const Set &input) {
        inputs_.at(index).assign_sorted(input.data(), input.size());
    }
    size_t universe_size() const { return graph_->universe().size(); }
    // Fixed Words must match this region; generated code dispatches once.
    template<size_t Words = 0>
    size_t materialize_local(size_t destination, size_t source, uint32_t position,
                             bool subtract, bool bounded, bool bound_only = false, bool remove_only = false) {
        if (!graph_->has_universe_rows()) throw std::logic_error("Requires universe rows");
        const auto *row = graph_->row_data_at(position);
        const auto &input = inputs_.at(source);
        inputs_.at(destination).assign_local<Words>(input, bound_only ? input.words().data() : row,
            subtract, bounded ? position : graph_->universe().size(),
            (subtract || remove_only) ? std::optional<uint32_t>(position) : std::nullopt);
        return inputs_[destination].count();
    }
    // Includes persistent row words, candidate words, ID mappings and object
    // storage plus one construction scratch bitmap. Allocator overhead is not
    // an exact resident-memory guarantee. Reserves room for one candidate state;
    // concurrent task forks have additional private storage, as before.
    // Zero budget forces array fallback. Rows can outlive any candidate state.
    template <class Graph, class Set>
    static std::shared_ptr<const BitGraph>
    build_rows(const Graph &graph, uint32_t anchor, const Set &neighbors, const Set &rows, size_t input_count,
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
        if (!charge(1, sizeof(BitmapCountRegion)) || !charge(1, sizeof(BitGraph)) ||
            !charge(neighbors.size(), sizeof(uint32_t)) ||
            !charge(rows.size(), sizeof(uint32_t)) || !charge(rows.size(), stride) ||
            !charge(input_count, stride) || !charge(input_count, sizeof(Bitmap)) || !charge(1, stride))
            return {};
        return std::make_shared<BitGraph>(
            NeighborhoodUniverse(anchor, neighbors.data(), neighbors.size()),
            std::vector<uint32_t>(rows.data(), rows.data() + rows.size()),
            [&](uint32_t vertex) { return graph.N(vertex); });
    }
    // Fresh private masks over shared immutable rows. A rejected row build
    // propagates the existing array fallback without allocating candidate state.
    static std::unique_ptr<BitmapCountRegion>
    from_rows(std::shared_ptr<const BitGraph> rows, size_t input_count) {
        if (!rows) return {};
        return std::unique_ptr<BitmapCountRegion>(new BitmapCountRegion(std::move(rows), input_count));
    }
    template <class Graph, class Set>
    static std::unique_ptr<BitmapCountRegion>
    build(const Graph &graph, uint32_t anchor, const Set &neighbors, const Set &rows, size_t input_count,
          size_t budget = 32 * 1024 * 1024) {
        return from_rows(build_rows(graph, anchor, neighbors, rows, input_count, budget), input_count);
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
        if (!graph_->has_universe_rows())
            throw std::logic_error("Local iteration requires universe-indexed rows");
        return {inputs_.at(input).words().data(), graph_->universe().size()};
    }
    BitmapLocalCursor local_cursor(size_t) const && = delete;
    // Canonicality against the selected local vertex is order-preserving because
    // universe IDs are sorted. Exclusion also stays entirely in local coordinates.
    template<size_t Words = 0>
    size_t count_local(size_t input, uint32_t position, bool subtract, bool bounded = false,
                       bool unary = false, bool remove_only = false) const {
        if (!graph_->has_universe_rows())
            throw std::logic_error("Local counting requires universe-indexed rows");
        const auto *source = inputs_.at(input).words().data();
        const auto *row = graph_->row_data_at(position);
        const auto bits = graph_->universe().size();
        const size_t limit = bounded ? position : bits;
        if (unary) {
            auto count = bit_ops::combine_fixed<Words, bit_ops::Binary::Intersection, false>(source, source, bits, nullptr, limit);
            if (remove_only && position < limit && bit_ops::test(source, bits, position)) --count;
            return count;
        }
        if (!subtract)
            return bit_ops::combine_fixed<Words, bit_ops::Binary::Intersection, false>(source, row, bits, nullptr, limit);
        size_t count = bit_ops::combine_fixed<Words, bit_ops::Binary::Difference, false>(source, row, bits, nullptr, limit);
        if (position < limit && bit_ops::test(source, bits, position) &&
            !bit_ops::test(row, bits, position))
            --count;
        return count;
    }
    size_t count(size_t input, uint32_t vertex, bool subtract,
                 std::optional<uint32_t> upper = {}) const {
        const auto source = inputs_.at(input).view();
        const auto row = graph_->row(vertex);
        return subtract ? source.subtract_count(row, vertex, upper) : source.intersect_count(row, upper);
    }
    size_t row_count() const { return graph_->row_count(); }
};
} // namespace minigraph
