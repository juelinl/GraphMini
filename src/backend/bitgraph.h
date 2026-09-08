#pragma once
#include "bitmap.h"

namespace minigraph {
// Initial eager immutable row store. Row vertices may differ from the column
// universe. Provider(vertex) returns a sorted adjacency with data()/size().
// Views remain valid until this BitGraph is destroyed or assigned; construction
// is the only mutation. No automatic selection or caching policy yet.
class BitGraph {
    NeighborhoodUniverse universe_;
    std::vector<uint32_t> rows_;
    std::vector<bit_ops::Word> words_;
    bool universe_rows_{false};

  public:
    template <class Neighbors>
    BitGraph(NeighborhoodUniverse universe, std::vector<uint32_t> rows, Neighbors neighbors)
        : universe_(std::move(universe)), rows_(std::move(rows)) {
        internal::require_sorted_ids(rows_.data(), rows_.size());
        universe_rows_ = rows_ == universe_.ids();
        const size_t stride = bit_ops::word_count(universe_.size());
        if (stride && rows_.size() > words_.max_size() / stride)
            throw std::length_error("BitGraph is too large");
        words_.resize(rows_.size() * stride);
        Bitmap bitmap(universe_);
        for (size_t i = 0; i < rows_.size(); ++i) {
            decltype(auto) adjacency = neighbors(rows_[i]);
            bitmap.assign_neighbors(adjacency.data(), adjacency.size());
            if (stride)
                std::copy(bitmap.words().begin(), bitmap.words().end(), words_.data() + i * stride);
        }
    }
    const NeighborhoodUniverse &universe() const { return universe_; }
    size_t row_count() const { return rows_.size(); }
    bool has_universe_rows() const { return universe_rows_; }
    size_t storage_bytes() const { return words_.size() * sizeof(bit_ops::Word); }
    const bit_ops::Word *row_data_at(size_t row) const & {
        if (row >= rows_.size())
            throw std::out_of_range("BitGraph row index");
        const size_t stride = bit_ops::word_count(universe_.size());
        return stride ? words_.data() + row * stride : nullptr;
    }
    const bit_ops::Word *row_data_at(size_t) const && = delete;
    BitmapView row_at(size_t row) const & {
        return {universe_, row_data_at(row), bit_ops::word_count(universe_.size())};
    }
    BitmapView row_at(size_t) const && = delete;
    BitmapView row(uint32_t vertex) const & {
        const auto it = std::lower_bound(rows_.begin(), rows_.end(), vertex);
        if (it == rows_.end() || *it != vertex)
            throw std::out_of_range("Unknown BitGraph row vertex");
        return row_at(static_cast<size_t>(it - rows_.begin()));
    }
    BitmapView row(uint32_t) const && = delete;
};
} // namespace minigraph
