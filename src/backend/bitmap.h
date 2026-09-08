#pragma once
#include <cstddef>
#include <cstdint>
#include "bit_ops/bit_ops.h"
#include "bit_ops/from_sorted.h"
#include "neighborhood_universe.h"

namespace minigraph {
class Bitmap;
// Scalar local-index iteration without materialization or global-ID lookup.
// Words are borrowed; do not mutate/rebind the owner while traversing a cursor.
class BitmapLocalCursor {
    const bit_ops::Word *words_{nullptr};
    size_t bits_{0}, word_{0};
    bit_ops::Word remaining_{0};
    void seek() {
        while (word_ < bit_ops::word_count(bits_)) {
            remaining_ = words_[word_] & bit_ops::word_mask(word_, bits_);
            if (remaining_)
                return;
            ++word_;
        }
    }

  public:
    BitmapLocalCursor() = default;
    BitmapLocalCursor(const bit_ops::Word *words, size_t bits) : words_(words), bits_(bits) {
        if (bits > std::numeric_limits<uint32_t>::max() || (bits && !words))
            throw std::invalid_argument("Invalid local bitmap cursor buffer");
        seek();
    }
    bool valid() const { return remaining_ != 0; }
    uint32_t position() const {
        if (!valid())
            throw std::out_of_range("Exhausted bitmap cursor");
        return static_cast<uint32_t>(word_ * 64 + bit_ops::trailing_zeros(remaining_));
    }
    void advance() {
        if (!remaining_)
            return;
        remaining_ &= remaining_ - 1;
        if (!remaining_) {
            ++word_;
            seek();
        }
    }
};
// The mapping is retained; words are borrowed and must outlive this view. Views
// observe writes and current cardinality. Moving/assigning an owner invalidates
// its views; in-place reset/rebinding does not. No view may escape its owner.
class BitmapView {
    NeighborhoodUniverse universe_;
    const bit_ops::Word *words_;
    const size_t *cardinality_{nullptr};
    friend class Bitmap;
    BitmapView(NeighborhoodUniverse universe, const bit_ops::Word *words, size_t word_count,
               const size_t *cardinality)
        : BitmapView(std::move(universe), words, word_count) {
        cardinality_ = cardinality;
    }
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
    BitmapLocalCursor local_cursor() const { return {words_, universe_.size()}; }
    size_t count(std::optional<uint32_t> upper = {}) const {
        if (!upper && cardinality_)
            return *cardinality_;
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
    size_t cardinality_{0};
    friend class BitmapView;

  public:
    explicit Bitmap(NeighborhoodUniverse universe, bool full = false)
        : universe_(std::move(universe)),
          words_(bit_ops::word_count(universe_.size()), full ? ~bit_ops::Word{0} : 0),
          cardinality_(full ? universe_.size() : 0) {
        if (!words_.empty())
            words_.back() &= bit_ops::word_mask(words_.size() - 1, universe_.size());
    }
    BitmapView view() const & { return {universe_, words_.data(), words_.size(), &cardinality_}; }
    BitmapView view() const && = delete;
    const NeighborhoodUniverse &universe() const { return universe_; }
    const std::vector<bit_ops::Word> &words() const { return words_; }
    size_t count() const { return cardinality_; }
    // Internal-region operation: fixed universe, preallocated destination.
    // Exact source/destination alias is supported by bit_ops.
    void assign_local(const Bitmap &source, const bit_ops::Word *row, bool subtract,
                      size_t limit, std::optional<uint32_t> excluded = {}) {
        if (!universe_.compatible(source.universe_))
            throw std::invalid_argument("Incompatible bitmap assignment");
        const size_t bits = universe_.size();
        cardinality_ = subtract
            ? bit_ops::difference_write(source.words_.data(), row, bits, words_.data(), limit)
            : bit_ops::intersection_write(source.words_.data(), row, bits, words_.data(), limit);
        if (excluded && bit_ops::test(words_.data(), bits, *excluded)) {
            bit_ops::clear(words_.data(), bits, *excluded);
            --cardinality_;
        }
    }
    void reset() {
        std::fill(words_.begin(), words_.end(), 0);
        cardinality_ = 0;
    }
    void set(uint32_t vertex) {
        const auto pos = universe_.position(vertex);
        if (!pos)
            throw std::out_of_range("Vertex is outside bitmap universe");
        if (!bit_ops::test(words_.data(), universe_.size(), *pos)) {
            words_[*pos / 64] |= bit_ops::Word{1} << (*pos % 64);
            ++cardinality_;
        }
    }
    void clear(uint32_t vertex) {
        if (auto pos = universe_.position(vertex)) {
            if (bit_ops::test(words_.data(), universe_.size(), *pos)) {
                bit_ops::clear(words_.data(), universe_.size(), *pos);
                --cardinality_;
            }
        }
    }
    // Rebind without reallocating words. Invalid containment leaves an empty
    // bitmap; malformed input is rejected before mutation.
    void assign_sorted(const uint32_t *ids, size_t size) {
        internal::require_sorted_ids(ids, size);
        reset();
        const auto &domain = universe_.ids();
        auto cursor = domain.begin();
        for (size_t i = 0; i < size; ++i) {
            cursor = std::lower_bound(cursor, domain.end(), ids[i]);
            if (cursor == domain.end() || *cursor != ids[i]) {
                reset();
                throw std::out_of_range("Vertex is outside bitmap universe");
            }
            const size_t pos = static_cast<size_t>(cursor - domain.begin());
            words_[pos / 64] |= bit_ops::Word{1} << (pos % 64);
            ++cardinality_;
            ++cursor;
        }
    }
    // Conversion requires a containment proof; reject rather than truncate.
    static Bitmap from_sorted(NeighborhoodUniverse universe, const uint32_t *ids, size_t size) {
        Bitmap out(std::move(universe));
        out.assign_sorted(ids, size);
        return out;
    }
    // Adjacency restriction is explicitly different from lossless conversion.
    static Bitmap from_neighbors(NeighborhoodUniverse universe, const uint32_t *ids, size_t size) {
        Bitmap out(std::move(universe));
        out.assign_neighbors(ids, size);
        return out;
    }
    void assign_neighbors(const uint32_t *ids, size_t size) {
        internal::require_sorted_ids(ids, size);
        const auto &domain = universe_.ids();
        cardinality_ = bit_ops::from_sorted(domain.data(), domain.size(), ids, size, words_.data());
    }
};
inline Bitmap BitmapView::intersect(const BitmapView &other, std::optional<uint32_t> upper) const {
    require_compatible(other);
    Bitmap out(universe_);
    out.cardinality_ = bit_ops::intersection_write(words_, other.words_, universe_.size(),
                                                   out.words_.data(), limit(upper));
    return out;
}
inline Bitmap BitmapView::difference(const BitmapView &other, std::optional<uint32_t> upper) const {
    require_compatible(other);
    Bitmap out(universe_);
    out.cardinality_ = bit_ops::difference_write(words_, other.words_, universe_.size(),
                                                 out.words_.data(), limit(upper));
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
    out.cardinality_ =
        bit_ops::intersection_write(words_, words_, universe_.size(), out.words_.data(), limit(upper));
    return out;
}
} // namespace minigraph
