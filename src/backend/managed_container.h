#pragma once
#include <cstddef>
#include <cstdint>
#include "minigraph_pool.h"
#include <utility>

namespace minigraph {
// Owning scratch storage, not a vector: indexing is capacity-based; Resize
// preserves data, whereas Reserve may discard data when it grows the buffer.
// Owners stay on their originating worker. Borrowed pointers are invalidated
// by growth/destruction and do not extend the owner's lifetime.
class ManagedContainer {
    MiniGraphPool* m_pool{nullptr};
    Container m_ctn;
    size_t m_size{0};
public:
    ManagedContainer() = default;
    explicit ManagedContainer(size_t size) {
        auto& pool = MiniGraphPool::Get();
        m_ctn = pool.AllocateWorkSpace(size);
        m_pool = &pool;
        m_size = size;
    }
    ~ManagedContainer() { if (m_pool) m_pool->FreeWorkSpace(m_ctn); }
    ManagedContainer(const ManagedContainer&) = delete;
    ManagedContainer& operator=(const ManagedContainer&) = delete;
    void swap(ManagedContainer& other) noexcept {
        std::swap(m_pool, other.m_pool);
        std::swap(m_ctn, other.m_ctn);
        std::swap(m_size, other.m_size);
    }
    ManagedContainer(ManagedContainer&& other) noexcept { swap(other); }
    ManagedContainer& operator=(ManagedContainer&& other) noexcept { swap(other); return *this; }
    void set_size(size_t size) {
        if (size > capacity()) throw std::length_error("ManagedContainer size exceeds capacity");
        m_size = size;
    }
    size_t size() const { return m_size; }
    size_t capacity() const { return m_ctn.capacity; }
    uint32_t* begin() { return m_ctn.data; }
    const uint32_t* begin() const { return m_ctn.data; }
    uint32_t* end() { return m_size ? m_ctn.data + m_size : m_ctn.data; }
    const uint32_t* end() const { return m_size ? m_ctn.data + m_size : m_ctn.data; }
    uint32_t& operator[](size_t i) { return m_ctn[i]; }
    const uint32_t& operator[](size_t i) const { return m_ctn[i]; }
    void Resize(size_t capacity) {
        if (capacity <= m_ctn.capacity) return;
        auto& pool = m_pool ? *m_pool : MiniGraphPool::Get();
        pool.Resize(m_ctn, capacity);
        m_pool = &pool;
    }
    void Reserve(size_t capacity) {
        if (capacity <= m_ctn.capacity) return;
        auto& pool = m_pool ? *m_pool : MiniGraphPool::Get();
        Container out = pool.AllocateWorkSpace(capacity);
        pool.FreeWorkSpace(m_ctn);
        m_ctn = out;
        m_pool = &pool;
    }
};
}
