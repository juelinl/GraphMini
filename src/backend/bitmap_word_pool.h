#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>

namespace minigraph::internal {
// A bounded cache of FREE buffers, not ownership of live allocations. Moving a
// bitmap across workers is safe: its buffer returns to the releasing worker's
// cache. Active/reentrant nested tasks always hold distinct unique_ptr leases.
class BitmapWordPool {
  public:
    static constexpr size_t alignment = 64;
    struct AlignedDelete {
        void operator()(uint64_t *buffer) const noexcept {
            ::operator delete[](buffer, std::align_val_t{alignment});
        }
    };
    using Buffer = std::unique_ptr<uint64_t[], AlignedDelete>;
  private:
    inline static thread_local bool cache_alive_ = true;
    std::map<size_t, std::vector<Buffer>> free_;
    size_t cached_bytes_ = 0;
  public:
    ~BitmapWordPool() { cache_alive_ = false; }
    static constexpr size_t cache_limit_bytes = 1024 * 1024;
    static constexpr size_t buffers_per_bucket = 8;
    static constexpr size_t max_capacity_bins = 128;
    static size_t capacity_for(size_t words) {
        constexpr size_t maximum = std::numeric_limits<size_t>::max() / sizeof(uint64_t);
        if (!words || words > maximum - 7) throw std::length_error("Invalid bitmap word capacity");
        return (words + 7) & ~size_t{7};
    }
    static BitmapWordPool &local() {
        static thread_local BitmapWordPool cache;
        return cache;
    }
    static void recycle(Buffer buffer, size_t capacity) noexcept {
        // A thread-local bitmap may be destroyed after its cache at thread exit.
        // In that case free directly rather than reentering a destroyed cache.
        if (cache_alive_) local().release(std::move(buffer), capacity);
    }
    Buffer acquire(size_t capacity) {
        if (capacity_for(capacity) != capacity) throw std::invalid_argument("Unaligned bitmap capacity");
        auto found = free_.find(capacity);
        if (found == free_.end() || found->second.empty())
            return Buffer(new (std::align_val_t{alignment}) uint64_t[capacity]);
        auto &list = found->second;
        auto result = std::move(list.back());
        list.pop_back();
        cached_bytes_ -= capacity * sizeof(uint64_t);
        return result;
    }
    void release(Buffer buffer, size_t capacity) noexcept {
        if (!buffer) return;
        const size_t bytes = capacity * sizeof(uint64_t);
        if (bytes > cache_limit_bytes - cached_bytes_) return;
        try {
            if (free_.find(capacity) == free_.end() && free_.size() >= max_capacity_bins) return;
            auto &list = free_[capacity];
            if (list.size() >= buffers_per_bucket) return;
            list.push_back(std::move(buffer));
            cached_bytes_ += bytes;
        } catch (...) {
            auto found = free_.find(capacity);
            if (found != free_.end() && found->second.empty()) free_.erase(found);
        }
    }
    size_t cached_bytes() const noexcept { return cached_bytes_; }
    void trim() noexcept {
        free_.clear();
        cached_bytes_ = 0;
    }
};
} // namespace minigraph::internal
