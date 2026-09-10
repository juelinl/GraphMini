#pragma once
#include <cstddef>
#include <cstdint>
#include "bit_ops/bit_ops.h"
#include "bit_ops/from_sorted.h"
#include "neighborhood_universe.h"
#include "bitmap_words.h"

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
    BitmapLocalCursor(const bit_ops::Word *words, size_t bits, size_t begin, size_t end)
        : words_(words), bits_(std::min(bits, end)), word_(begin / 64) {
        if (begin > end || end > bits || bits > std::numeric_limits<uint32_t>::max() || (bits && !words))
            throw std::invalid_argument("Invalid local bitmap cursor range");
        if (begin == end) return;
        seek();
        if (word_ == begin / 64) {
            remaining_ &= (~bit_ops::Word{0}) << (begin % 64);
            if (!remaining_) { ++word_; seek(); }
        }
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
// A local BitGraph row borrows both words and universe. The immutable BitGraph
// must outlive the view (including synchronous task joins). Unlike BitmapView,
// creating this hot-loop view does not increment a shared ownership counter.
class BitmapRowView {
    const NeighborhoodUniverse *universe_;
    const bit_ops::Word *words_;
    size_t cardinality_;
    friend class BitGraph;
    BitmapRowView(const NeighborhoodUniverse &universe, const bit_ops::Word *words, size_t cardinality)
        : universe_(&universe), words_(words), cardinality_(cardinality) {}
  public:
    const NeighborhoodUniverse &universe() const { return *universe_; }
    const bit_ops::Word *data() const { return words_; }
    size_t count() const { return cardinality_; }
    size_t capacity_bound() const { return cardinality_; }
};

// The mapping is retained; words are borrowed and must outlive this view. Views
// observe writes and current cardinality. Moving/assigning an owner invalidates
// its views; in-place reset/rebinding does not. No view may escape its owner.
class BitmapView {
    NeighborhoodUniverse universe_;
    const bit_ops::Word *words_;
    const size_t *cardinality_{nullptr};
    const bool *count_exact_{nullptr};
    friend class Bitmap;
    friend class BitGraph;
    BitmapView(NeighborhoodUniverse universe, const bit_ops::Word *words, size_t word_count,
               const size_t *cardinality, const bool *count_exact = nullptr)
        : BitmapView(std::move(universe), words, word_count) {
        cardinality_ = cardinality;
        count_exact_ = count_exact;
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
        if (!upper && cardinality_ && (!count_exact_ || *count_exact_))
            return *cardinality_;
        return bit_ops::count(words_, universe_.size(), limit(upper));
    }
    size_t capacity_bound() const { return cardinality_ ? *cardinality_ : universe_.size(); }
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

// Owned words, inline through 512 bits. Copying is deep; moving copies inline
// words or transfers large storage. A moved-from Bitmap may only be assigned
// or destroyed. Large word buffers recycle through a bounded worker-local pool.
class Bitmap {
    NeighborhoodUniverse universe_;
    internal::BitmapWords<8> words_;
    size_t cardinality_{0};
    // If false, cardinality_ is only an upper bound. Const reads never update
    // this metadata: parent inputs can be borrowed concurrently by child tasks.
    bool count_exact_{true};
    friend class BitmapView;

  public:
    explicit Bitmap(NeighborhoodUniverse universe, bool full = false)
        : universe_(std::move(universe)),
          words_(bit_ops::word_count(universe_.size()), full ? ~bit_ops::Word{0} : 0),
          cardinality_(full ? universe_.size() : 0) {
        if (!words_.empty())
            words_.back() &= bit_ops::word_mask(words_.size() - 1, universe_.size());
    }
    BitmapView view() const & { return {universe_, words_.data(), words_.size(), &cardinality_, &count_exact_}; }
    BitmapView view() const && = delete;
    const NeighborhoodUniverse &universe() const { return universe_; }
    const internal::BitmapWords<8> &words() const { return words_; }
    size_t count() const {
        return count_exact_ ? cardinality_ : bit_ops::count(words_.data(), universe_.size());
    }
    size_t capacity_bound() const { return cardinality_; }
    bool has_exact_count() const { return count_exact_; }
    bool empty() const { return count_exact_ ? !cardinality_ : !local_cursor().valid(); }
    BitmapLocalCursor local_cursor() const & { return {words_.data(), universe_.size()}; }
    BitmapLocalCursor local_cursor(size_t begin, size_t end) const & {
        return {words_.data(), universe_.size(), begin, end};
    }
    BitmapLocalCursor local_cursor() const && = delete;
    BitmapLocalCursor local_cursor(size_t, size_t) const && = delete;
    // Generated operations use LOCAL positions for bounds and exclusions.
    // Destinations are preallocated and can alias the source exactly.
    // Count=false writes only, retaining a proven upper bound for later decode.
    template<size_t Words = 0, bool Count = true, class Row>
    void assign_intersection(const Bitmap &source, const Row &row, size_t upper = bit_ops::unlimited) {
        require_row(row);
        size_t row_bound = bit_ops::unlimited;
        if constexpr (!Count) row_bound = row.capacity_bound();
        assign_local<Words, Count>(source, row.data(), false, upper, {}, row_bound);
    }
    template<size_t Words = 0, bool Count = true, class Row>
    void assign_subtraction(const Bitmap &source, const Row &row, uint32_t excluded,
                            size_t upper = bit_ops::unlimited) {
        require_row(row);
        assign_local<Words, Count>(source, row.data(), true, upper, excluded);
    }
    template<size_t Words = 0, bool Count = true>
    void assign_bounded(const Bitmap &source, size_t upper) {
        assign_local<Words, Count>(source, source.words_.data(), false, upper);
    }
    template<size_t Words = 0, bool Count = true>
    void assign_removed(const Bitmap &source, uint32_t excluded, size_t upper = bit_ops::unlimited) {
        assign_local<Words, Count>(source, source.words_.data(), false, upper, excluded);
    }
    template<size_t Words = 0, class Row>
    size_t intersection_count(const Row &row, size_t upper = bit_ops::unlimited) const {
        require_row(row);
        return bit_ops::combine_fixed<Words, bit_ops::Binary::Intersection, false>(
            words_.data(), row.data(), universe_.size(), nullptr, upper);
    }
    template<size_t Words = 0, class Row>
    size_t subtraction_count(const Row &row, uint32_t excluded, size_t upper = bit_ops::unlimited) const {
        require_row(row);
        auto result = bit_ops::combine_fixed<Words, bit_ops::Binary::Difference, false>(
            words_.data(), row.data(), universe_.size(), nullptr, upper);
        if (excluded < upper && bit_ops::test(words_.data(), universe_.size(), excluded) &&
            !bit_ops::test(row.data(), universe_.size(), excluded)) --result;
        return result;
    }
    template<size_t Words = 0>
    size_t bounded_count(size_t upper) const {
        return bit_ops::combine_fixed<Words, bit_ops::Binary::Intersection, false>(
            words_.data(), words_.data(), universe_.size(), nullptr, upper);
    }
    template<size_t Words = 0>
    size_t removed_count(uint32_t excluded, size_t upper = bit_ops::unlimited) const {
        auto result = bounded_count<Words>(upper);
        if (excluded < upper && bit_ops::test(words_.data(), universe_.size(), excluded)) --result;
        return result;
    }
    // Internal-region operation: fixed universe, preallocated destination.
    // Exact source/destination alias is supported by bit_ops.
    template<size_t Words = 0, bool Count = true>
    void assign_local(const Bitmap &source, const bit_ops::Word *row, bool subtract,
                      size_t limit, std::optional<uint32_t> excluded = {},
                      size_t row_bound = bit_ops::unlimited) {
        if (!universe_.compatible(source.universe_))
            throw std::invalid_argument("Incompatible bitmap assignment");
        const size_t bits = universe_.size();
        // Read bounds before writing: source and even row may alias this output.
        const size_t bound = std::min({source.capacity_bound(), bits, limit,
                                      subtract ? bits : row_bound});
        const auto written_count = subtract
            ? bit_ops::combine_fixed<Words, bit_ops::Binary::Difference, true, Count>(source.words_.data(), row, bits, words_.data(), limit)
            : bit_ops::combine_fixed<Words, bit_ops::Binary::Intersection, true, Count>(source.words_.data(), row, bits, words_.data(), limit);
        cardinality_ = Count ? written_count : bound;
        count_exact_ = Count || !bound;
        if (excluded && bit_ops::test(words_.data(), bits, *excluded)) {
            bit_ops::clear(words_.data(), bits, *excluded);
            --cardinality_;
        }
    }
    void reset() {
        std::fill(words_.begin(), words_.end(), 0);
        cardinality_ = 0;
        count_exact_ = true;
    }
    void set(uint32_t vertex) {
        const auto pos = universe_.position(vertex);
        if (!pos)
            throw std::out_of_range("Vertex is outside bitmap universe");
        if (!bit_ops::test(words_.data(), universe_.size(), *pos)) {
            words_[*pos / 64] |= bit_ops::Word{1} << (*pos % 64);
            cardinality_ = std::min(universe_.size(), cardinality_ + 1);
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
    void assign_neighbors(const uint32_t *ids, size_t size, std::optional<uint32_t> upper = {}) {
        internal::require_sorted_ids(ids, size);
        const auto &domain = universe_.ids();
        const auto limit = upper ? universe_.lower_bound(*upper) : domain.size();
        if (upper) {
            // Projection writes a shorter universe; erase the unused word tail
            // too, since this storage may contain an earlier prefix's bits.
            std::fill(words_.begin(), words_.end(), bit_ops::Word{0});
            if (size) size = std::lower_bound(ids, ids + size, *upper) - ids;
        }
        cardinality_ = bit_ops::from_sorted(domain.data(), limit, ids, size, words_.data());
        count_exact_ = true;
    }
  private:
    template<class Row> void require_row(const Row &row) const {
        if (!universe_.compatible(row.universe()))
            throw std::invalid_argument("Bitmap row universe mismatch");
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
