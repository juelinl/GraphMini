#pragma once
#include "vertex_set.h"
#include "bit_ops/bit_ops.h"
#include "bit_ops/from_sorted.h"
#include <array>
#include <initializer_list>
#include <vector>
#include <stdexcept>
#include <algorithm>

namespace minigraph {
// Local to one IEP evaluation. Arrays already encode injectivity/bounds; bitmap
// conversion preserves exactly those candidates. No BitGraph rows or decoding.
class IEPBitmap {
    size_t bits_, words_, num_rows_;
    bool enabled_{false};
    // Eight two-word rows (or sixteen one-word rows) need no heap allocation.
    std::array<bit_ops::Word, 16> inline_rows_;
    std::vector<bit_ops::Word> heap_rows_;
    bit_ops::Word *rows_;
public:
    IEPBitmap(const VertexSet &universe, std::initializer_list<const VertexSet *> sets,
              size_t max_bytes = 32 * 1024 * 1024)
        : bits_(universe.size()), words_(bit_ops::word_count(bits_)), num_rows_(sets.size()) {
        if (words_ > max_bytes / sizeof(bit_ops::Word) / (sets.size() + 1)) return;
        const size_t storage = words_ * sets.size();
        if (storage > inline_rows_.size()) heap_rows_.resize(storage);
        rows_ = storage > inline_rows_.size() ? heap_rows_.data() : inline_rows_.data();
        size_t row = 0;
        for (const auto *set : sets) {
            const auto count = bit_ops::from_sorted(universe.begin(), bits_, set->begin(),
                                                    set->size(), rows_ + row * words_);
            if (count != set->size())
                throw std::logic_error("IEP candidate outside proven universe");
            ++row;
        }
        enabled_ = true;
    }
    // rows_ points into this object's storage; copying must not borrow it.
    IEPBitmap(const IEPBitmap &) = delete;
    IEPBitmap &operator=(const IEPBitmap &) = delete;
    bool enabled() const { return enabled_; }
    size_t intersection_count(std::initializer_list<size_t> slots) {
        if (!enabled_ || slots.size() < 2) throw std::logic_error("Invalid IEP bitmap factor");
        for (auto slot : slots)
            if (slot >= num_rows_) throw std::out_of_range("Invalid IEP bitmap slot");
        if (!bits_) return 0;
        auto it = slots.begin();
        const auto *first = rows_ + *it++ * words_;
        if (slots.size() == 2)
            return bit_ops::intersection_count(first, rows_ + *it * words_, bits_);
        size_t count = 0;
        // Fuse all inputs before the single popcount. Rows have zero tail bits.
        for (size_t word = 0; word < words_; ++word) {
            auto value = first[word];
            for (auto rhs = it; rhs != slots.end(); ++rhs)
                value &= rows_[*rhs * words_ + word];
            count += bit_ops::popcount(value);
        }
        return count;
    }
};
} // namespace minigraph
