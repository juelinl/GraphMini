#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace minigraph {
struct Container {
    uint32_t* data{nullptr};
    size_t capacity{0};
    Container() = default;
    Container(uint32_t* data, size_t capacity) : data(data), capacity(capacity) {}
    uint32_t& operator[](size_t i) { assert(i < capacity); return data[i]; }
    const uint32_t& operator[](size_t i) const { assert(i < capacity); return data[i]; }
};

// Variable-capacity, thread-confined workspace. Free lists retain buffers until
// worker exit; raw Container handles are non-owning and must be returned once.
class MiniGraphPool {
    static constexpr size_t page_elements = 4096 / sizeof(uint32_t);
    std::vector<std::unique_ptr<uint32_t[]>> owned_;
    std::vector<Container> small_, large_;
#ifndef NDEBUG
    const std::thread::id thread_ = std::this_thread::get_id();
#endif
    void check_thread() const noexcept {
#ifndef NDEBUG
        assert(thread_ == std::this_thread::get_id());
#endif
    }
    static size_t allocation_capacity(size_t requested) {
        constexpr size_t limit = std::numeric_limits<size_t>::max() / sizeof(uint32_t);
        if (requested > limit) throw std::length_error("MiniGraph capacity overflow");
        if (requested <= page_elements) return page_elements;
        // Approximately 1.5x growth, rounded to a page, without floating-point
        // conversion or integer overflow. Near the limit, use exact capacity.
        if (requested > limit - requested / 2) return requested;
        const size_t grown = requested + requested / 2;
        const size_t padding = page_elements - grown % page_elements;
        return grown <= limit - padding ? grown + padding : requested;
    }
public:
    // Newly allocated bytes since reset, not currently resident bytes.
    inline static std::atomic_uint64_t TOTAL_ALLOCATED{0};
    MiniGraphPool() = default;
    MiniGraphPool(const MiniGraphPool&) = delete;
    MiniGraphPool& operator=(const MiniGraphPool&) = delete;
    ~MiniGraphPool() { assert(checked_out() == 0 && "Container outlived its worker pool"); }
    size_t checked_out() const noexcept { return owned_.size() - small_.size() - large_.size(); }
    size_t buffer_count() const noexcept { return owned_.size(); }
    static MiniGraphPool& Get() { static thread_local MiniGraphPool pool; return pool; }

    Container AllocateWorkSpace(size_t requested) {
        check_thread();
        const size_t capacity = allocation_capacity(requested);
        auto& free = requested <= page_elements ? small_ : large_;
        auto it = requested <= page_elements ? (free.empty() ? free.end() : free.end() - 1) :
            std::lower_bound(free.begin(), free.end(), requested,
                [](const Container& c, size_t n) { return c.capacity < n; });
        if (it != free.end()) {
            Container out = *it;
            free.erase(it);
            return out;
        }
        // Reserve before acquisition so returning a buffer cannot allocate.
        const size_t n = owned_.size() + 1;
        if (free.capacity() < n) {
            const size_t grown = free.capacity() <= free.max_size() / 2
                ? free.capacity() * 2 : free.max_size();
            free.reserve(std::max(n, grown));
        }
        std::unique_ptr<uint32_t[]> data(new uint32_t[capacity]);
        Container out(data.get(), capacity);
        owned_.push_back(std::move(data));
        TOTAL_ALLOCATED.fetch_add(capacity * sizeof(uint32_t), std::memory_order_relaxed);
        return out;
    }
    void FreeWorkSpace(Container c) noexcept {
        if (!c.data) return;
        check_thread();
        assert(checked_out() > 0);
        if (c.capacity <= page_elements) {
            small_.push_back(c);
        } else {
            auto it = std::lower_bound(large_.begin(), large_.end(), c.capacity,
                [](const Container& x, size_t n) { return x.capacity < n; });
            large_.insert(it, c);
        }
    }
    // Growth only. Scratch users write beyond logical size, so preserve the
    // entire previous capacity as object bytes, including uninitialized slots.
    void Resize(Container& c, size_t capacity) {
        check_thread();
        if (c.data && capacity <= c.capacity) return;
        Container out = AllocateWorkSpace(capacity);
        if (c.data) std::memcpy(out.data, c.data, c.capacity * sizeof(uint32_t));
        FreeWorkSpace(c);
        c = out;
    }
};
}
