#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace minigraph::internal {

// Fixed-size, thread-confined storage. Pools and owning sets must remain on
// their originating worker; borrowed views may be read by other workers while
// the owner remains alive. No per-buffer capacity lookup is needed.
class VertexSetPool {
    const size_t capacity_;
    std::atomic_uint64_t* allocated_;
    std::vector<std::unique_ptr<uint32_t[]>> owned_;
    std::vector<uint32_t*> available_;
#ifndef NDEBUG
    const std::thread::id thread_ = std::this_thread::get_id();
#endif
public:
    VertexSetPool(size_t capacity, std::atomic_uint64_t& allocated)
        : capacity_(capacity), allocated_(&allocated) {
        if (!capacity || capacity > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
            throw std::length_error("Invalid VertexSetPool capacity");
    }
    VertexSetPool(const VertexSetPool&) = delete;
    VertexSetPool& operator=(const VertexSetPool&) = delete;
    ~VertexSetPool() { assert(checked_out() == 0 && "VertexSet outlived its worker pool"); }

    size_t capacity() const noexcept { return capacity_; }
    size_t checked_out() const noexcept { return owned_.size() - available_.size(); }
    size_t buffer_count() const noexcept { return owned_.size(); }

    uint32_t* acquire() {
#ifndef NDEBUG
        assert(thread_ == std::this_thread::get_id());
#endif
        if (!available_.empty()) {
            auto* buffer = available_.back();
            available_.pop_back();
            return buffer;
        }
        // Reserve free-list space before handing out another buffer, so release
        // does not allocate or throw. All allocating operations precede mutation.
        const size_t n = owned_.size() + 1;
        available_.reserve(n);
        owned_.reserve(n);
        std::unique_ptr<uint32_t[]> buffer(new uint32_t[capacity_]);
        auto* out = buffer.get();
        owned_.push_back(std::move(buffer));
        allocated_->fetch_add(capacity_ * sizeof(uint32_t), std::memory_order_relaxed);
        return out;
    }

    void release(uint32_t* buffer) noexcept {
#ifndef NDEBUG
        assert(thread_ == std::this_thread::get_id());
#endif
        assert(buffer && checked_out() > 0);
        available_.push_back(buffer);
    }

    // A hot cached pool serves compatible requests, including later smaller
    // graphs. A larger request creates another fixed-capacity pool, never
    // resizes storage that might still have outstanding owners.
    static VertexSetPool& for_capacity(size_t capacity, std::atomic_uint64_t& allocated) {
        struct WorkerPools {
            std::vector<std::unique_ptr<VertexSetPool>> pools;
            VertexSetPool* current = nullptr;
        };
        static thread_local WorkerPools worker;
        if (worker.current && worker.current->capacity_ >= capacity &&
            worker.current->allocated_ == &allocated)
            return *worker.current;
        for (auto& pool : worker.pools) {
            if (pool->capacity_ >= capacity && pool->allocated_ == &allocated) {
                worker.current = pool.get();
                return *worker.current;
            }
        }
        auto pool = std::make_unique<VertexSetPool>(capacity, allocated);
        auto* selected = pool.get();
        worker.pools.push_back(std::move(pool));
        worker.current = selected;
        return *selected;
    }
};
}
