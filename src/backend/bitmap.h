#pragma once
#include "bit_ops/bit_ops.h"
#include "neighborhood_universe.h"

namespace minigraph {
class Bitmap;
// The mapping is retained; words are borrowed and must outlive this view. Views
// observe writes to those words. No view may escape its Bitmap/BitGraph owner.
class BitmapView {
    NeighborhoodUniverse universe_;
    const bit_ops::Word *words_;
    size_t limit(std::optional<uint32_t> upper) const {
        return upper ? universe_.lower_bound(*upper) : universe_.size();
    }
    void require_compatible(const BitmapView &other) const {
        if (!universe_.compatible(other.universe_))
            throw std::invalid_argument("Bitmap universe mismatch");
    }

  public:
    BitmapView(NeighborhoodUniverse universe, const bit_ops::Word *words, size_t word_count)
        : universe_(std::move(universe)), words_(words) {
        if (word_count != bit_ops::word_count(universe_.size()) || (word_count && !words))
            throw std::invalid_argument("Invalid bitmap word buffer");
    }
    const NeighborhoodUniverse &universe() const { return universe_; }
    const bit_ops::Word *data() const { return words_; }
    size_t count(std::optional<uint32_t> upper = {}) const {
        return bit_ops::count(words_, universe_.size(), limit(upper));
    }
    bool contains(uint32_t vertex) const {
        auto pos = universe_.position(vertex);
        return pos && bit_ops::test(words_, universe_.size(), *pos);
    }
    size_t intersect_count(const BitmapView &other, std::optional<uint32_t> upper = {}) const {
        require_compatible(other);
        return bit_ops::intersection_count(words_, other.words_, universe_.size(), limit(upper));
    }
    size_t difference_count(const BitmapView &other, std::optional<uint32_t> upper = {}) const {
        require_compatible(other);
        return bit_ops::difference_count(words_, other.words_, universe_.size(), limit(upper));
    }
    // GraphMini subtraction is difference AND exclusion of the RHS owner.
    // UINT32_MAX is a real vertex here, not a "no owner" sentinel.
    size_t subtract_count(const BitmapView &other, uint32_t excluded,
                          std::optional<uint32_t> upper = {}) const {
        size_t result = difference_count(other, upper);
        if ((!upper || excluded < *upper) && contains(excluded) && !other.contains(excluded))
            --result;
        return result;
    }
    template <class Visitor> void for_each(Visitor visit) const {
        bit_ops::for_each(words_, universe_.size(), [&](size_t pos) { visit(universe_.vertex(pos)); });
    }
    std::vector<uint32_t> vertices() const {
        std::vector<uint32_t> out;
        out.reserve(count());
        for_each([&](uint32_t vertex) { out.push_back(vertex); });
        return out;
    }
    Bitmap intersect(const BitmapView &other, std::optional<uint32_t> upper = {}) const;
    Bitmap difference(const BitmapView &other, std::optional<uint32_t> upper = {}) const;
    Bitmap subtract(const BitmapView &other, uint32_t excluded,
                    std::optional<uint32_t> upper = {}) const;
    Bitmap bounded(uint32_t upper) const;
};

// Owned words; copying is deep, moving transfers storage. A moved-from Bitmap
// may only be assigned or destroyed. No pool/global state in the initial path.
class Bitmap {
    NeighborhoodUniverse universe_;
    std::vector<bit_ops::Word> words_;
    friend class BitmapView;

  public:
    explicit Bitmap(NeighborhoodUniverse universe, bool full = false)
        : universe_(std::move(universe)),
          words_(bit_ops::word_count(universe_.size()), full ? ~bit_ops::Word{0} : 0) {
        if (!words_.empty())
            words_.back() &= bit_ops::word_mask(words_.size() - 1, universe_.size());
    }
    BitmapView view() const & { return {universe_, words_.data(), words_.size()}; }
    BitmapView view() const && = delete;
    const NeighborhoodUniverse &universe() const { return universe_; }
    const std::vector<bit_ops::Word> &words() const { return words_; }
    void set(uint32_t vertex) {
        const auto pos = universe_.position(vertex);
        if (!pos)
            throw std::out_of_range("Vertex is outside bitmap universe");
        words_[*pos / 64] |= bit_ops::Word{1} << (*pos % 64);
    }
    void clear(uint32_t vertex) {
        if (auto pos = universe_.position(vertex))
            bit_ops::clear(words_.data(), universe_.size(), *pos);
    }
    // Conversion requires a containment proof; reject rather than truncate.
    static Bitmap from_sorted(NeighborhoodUniverse universe, const uint32_t *ids, size_t size) {
        internal::require_sorted_ids(ids, size);
        Bitmap out(std::move(universe));
        for (size_t i = 0; i < size; ++i)
            out.set(ids[i]);
        return out;
    }
    // Adjacency restriction is explicitly different from lossless conversion.
    static Bitmap from_neighbors(NeighborhoodUniverse universe, const uint32_t *ids, size_t size) {
        internal::require_sorted_ids(ids, size);
        Bitmap out(std::move(universe));
        const auto &domain = out.universe_.ids();
        size_t i = 0, j = 0;
        while (i < domain.size() && j < size) {
            if (domain[i] < ids[j])
                ++i;
            else if (ids[j] < domain[i])
                ++j;
            else {
                out.words_[i / 64] |= bit_ops::Word{1} << (i % 64);
                ++i;
                ++j;
            }
        }
        return out;
    }
};
inline Bitmap BitmapView::intersect(const BitmapView &other, std::optional<uint32_t> upper) const {
    require_compatible(other);
    Bitmap out(universe_);
    bit_ops::intersection_write(words_, other.words_, universe_.size(), out.words_.data(), limit(upper));
    return out;
}
inline Bitmap BitmapView::difference(const BitmapView &other, std::optional<uint32_t> upper) const {
    require_compatible(other);
    Bitmap out(universe_);
    bit_ops::difference_write(words_, other.words_, universe_.size(), out.words_.data(), limit(upper));
    return out;
}
inline Bitmap BitmapView::subtract(const BitmapView &other, uint32_t excluded,
                                   std::optional<uint32_t> upper) const {
    auto out = difference(other, upper);
    out.clear(excluded);
    return out;
}
inline Bitmap BitmapView::bounded(uint32_t upper) const {
    Bitmap out(universe_);
    bit_ops::copy_prefix(words_, universe_.size(), out.words_.data(), limit(upper));
    return out;
}
} // namespace minigraph
