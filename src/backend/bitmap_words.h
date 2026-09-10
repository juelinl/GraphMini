#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include "bitmap_word_pool.h"

namespace minigraph::internal {
// Small-buffer storage, not a SIMD register type. No self-referential pointer:
// copy/move safely selects the destination's own inline array. Large allocations
// are uniquely owned leases returned to the current worker's bounded cache.
template<size_t InlineWords> class BitmapWords {
    static_assert(InlineWords == 1 || InlineWords == 2 || InlineWords == 4 || InlineWords == 8);
    std::array<uint64_t, InlineWords> inline_{};
    BitmapWordPool::Buffer heap_;
    size_t capacity_ = 0;
    size_t size_;
  public:
    BitmapWords(size_t size, uint64_t value = 0) : size_(size) {
        if (is_inline()) std::fill_n(inline_.data(), size_, value);
        else {
            capacity_ = BitmapWordPool::capacity_for(size_);
            heap_ = BitmapWordPool::local().acquire(capacity_);
            std::fill_n(heap_.get(), size_, value);
        }
    }
    ~BitmapWords() { if (heap_) BitmapWordPool::recycle(std::move(heap_), capacity_); }
    BitmapWords(const BitmapWords &other) : BitmapWords(other.size_) {
        std::copy_n(other.data(), size_, data());
    }
    BitmapWords(BitmapWords &&other) noexcept
        : inline_(other.inline_), heap_(std::move(other.heap_)),
          capacity_(other.capacity_), size_(other.size_) { other.size_ = 0; other.capacity_ = 0; }
    void swap(BitmapWords &other) noexcept {
        inline_.swap(other.inline_);
        heap_.swap(other.heap_);
        std::swap(capacity_, other.capacity_);
        std::swap(size_, other.size_);
    }
    BitmapWords &operator=(const BitmapWords &other) {
        if (this != &other) { BitmapWords copy(other); swap(copy); }
        return *this;
    }
    BitmapWords &operator=(BitmapWords &&other) noexcept {
        if (this != &other) { BitmapWords moved(std::move(other)); swap(moved); }
        return *this;
    }
    bool is_inline() const { return size_ <= InlineWords; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint64_t *data() { return is_inline() ? inline_.data() : heap_.get(); }
    const uint64_t *data() const { return is_inline() ? inline_.data() : heap_.get(); }
    uint64_t *begin() { return data(); }
    const uint64_t *begin() const { return data(); }
    uint64_t *end() { return data() + size_; }
    const uint64_t *end() const { return data() + size_; }
    uint64_t &operator[](size_t i) { return data()[i]; }
    const uint64_t &operator[](size_t i) const { return data()[i]; }
    uint64_t &back() { return data()[size_ - 1]; }
};
} // namespace minigraph::internal
