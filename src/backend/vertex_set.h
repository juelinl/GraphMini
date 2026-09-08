//
// Created by ubuntu on 2/2/23.
//

#ifndef MINIGRAPH_VERTEX_SET_H
#define MINIGRAPH_VERTEX_SET_H
#include <cstdint>
#include <vector>
#include <cassert>
#include <cstddef>
#include <atomic>
#include "vertex_set_pool.h"
#include "set_ops/set_ops.h"

namespace minigraph {
    using IdType = uint32_t;    // support up to 4-billion number of vertexes (2^64-1 edges)
    constexpr IdType INVALID_ID = static_cast<IdType>(-1);

    class VertexSet {
    private:
        internal::VertexSetPool* m_pool{nullptr};
        IdType *m_data{nullptr};
        IdType m_size{0};
        IdType m_vid{INVALID_ID};

        static IdType checked_size(uint64_t size) {
            if (size > std::numeric_limits<IdType>::max())
                throw std::length_error("VertexSet size exceeds IdType range");
            return static_cast<IdType>(size);
        }
    public:
        VertexSet() = default;

        VertexSet(IdType _vid, IdType *_data, uint64_t _size) :
                m_data{_data}, m_size{checked_size(_size)}, m_vid{_vid} {};

        VertexSet(size_t capacity) {
            m_pool = &internal::VertexSetPool::for_request(capacity);
            m_data = m_pool->acquire();
        };

        ~VertexSet() {
            if (m_pool) m_pool->release(m_data);
        };

        void swap(VertexSet &other) noexcept {
            std::swap(m_data, other.m_data);
            std::swap(m_size, other.m_size);
            std::swap(m_pool, other.m_pool);
            std::swap(m_vid, other.m_vid);
        };

        // reference to src / pointer copy
        VertexSet(const VertexSet &src) {
            m_data = src.m_data;
            m_size = src.m_size;
            m_vid = src.m_vid;
            m_pool = nullptr;
        };

        // reference to src / pointer copy
        VertexSet &operator=(const VertexSet &src) {
            if (&src != this) {
                VertexSet tmp{src};
                swap(tmp);
            }
            return *this;
        };

        VertexSet(VertexSet &&src) noexcept { swap(src); };

        VertexSet &operator=(VertexSet &&src) noexcept {
            swap(src);
            return *this;
        };

        // Preserve the public arithmetic type; only the stored count is narrowed.
        uint64_t size() const { return m_size; };
        IdType vid() const { return m_vid; };
        IdType *begin() { return m_data; };
        IdType *end() { return m_data + m_size; };
        bool pooled() const { return m_pool != nullptr; };
        const IdType *begin() const { return m_data; };
        const IdType *end() const { return m_data + m_size; };

        inline IdType &operator[](size_t i) {
            assert(i < m_size);
            return m_data[i];
        };

        inline IdType operator[](size_t i) const {
            assert(i < m_size);
            return m_data[i];
        };

        void set_size(size_t _size) { m_size = checked_size(_size); };

        inline VertexSet intersect(const VertexSet &other, IdType upper) const;
        inline VertexSet intersect(const VertexSet &other) const;
        inline size_t intersect(const VertexSet &other, IdType upper, IdType *buffer) const;
        inline size_t intersect(const VertexSet &other, IdType *buffer) const;
        inline size_t intersect_cnt(const VertexSet &other, IdType upper) const;
        inline size_t intersect_cnt(const VertexSet &other) const;
        inline VertexSet subtract(const VertexSet &other, IdType upper) const;
        inline VertexSet subtract(const VertexSet &other) const;
        inline size_t subtract_cnt(const VertexSet &other, IdType upper) const;
        inline size_t subtract_cnt(const VertexSet &other) const;
        inline VertexSet bounded(IdType upper) const &;
        inline VertexSet bounded(IdType upper) &&;
        inline size_t bounded_cnt(IdType upper) const;
        inline VertexSet remove(IdType id) const &;
        inline VertexSet remove(IdType id) &&;
        inline size_t remove_cnt(IdType id) const;
        inline VertexSet indices(const VertexSet &_vertex) const;
    };

    VertexSet VertexSet::intersect(const VertexSet &other) const {
        VertexSet out(size());
        out.m_size = set_ops::intersection_write(m_data, size(), other.m_data, other.size(), out.m_data);
        return out;
    };

    size_t VertexSet::intersect(const VertexSet &other, IdType *buffer) const {
        return set_ops::intersection_write(m_data, size(), other.m_data, other.size(), buffer);
    };

    VertexSet VertexSet::intersect(const VertexSet &other, IdType upper) const {
        VertexSet out(size());
        out.m_size = intersect(other, upper, out.m_data);
        return out;
    };

    size_t VertexSet::intersect(const VertexSet &other, IdType upper, IdType *buffer) const {
        return set_ops::intersection_bounded<true>(
            m_data, size(), other.m_data, other.size(), upper, buffer);
    };

    size_t VertexSet::intersect_cnt(const VertexSet &other) const {
        return set_ops::intersection_count(m_data, size(), other.m_data, other.size());
    };

    size_t VertexSet::intersect_cnt(const VertexSet &other, IdType upper) const {
        return set_ops::intersection_bounded<false>(
            m_data, size(), other.m_data, other.size(), upper);
    };

    VertexSet VertexSet::subtract(const VertexSet &other) const {
        VertexSet out(size());
        out.m_size = set_ops::difference_write(
            m_data, size(), other.m_data, other.size(), other.m_vid, out.m_data);
        return out;
    };

    VertexSet VertexSet::subtract(const VertexSet &other, IdType upper) const {
        VertexSet out(size());
        out.m_size = set_ops::difference_bounded<true>(
            m_data, size(), other.m_data, other.size(), other.m_vid, upper, out.m_data);
        return out;
    };

    size_t VertexSet::subtract_cnt(const VertexSet &other) const {
        return set_ops::difference_count(m_data, size(), other.m_data, other.size(), other.m_vid);
    };

    size_t VertexSet::subtract_cnt(const VertexSet &other, IdType upper) const {
        return set_ops::difference_bounded<false>(
            m_data, size(), other.m_data, other.size(), other.m_vid, upper);
    };

    VertexSet VertexSet::bounded(IdType upper) const & {
        size_t idx_l = 0;
        if (size() > 64) {
            size_t count = size();
            while (count > 0) {
                size_t it = idx_l;
                size_t step = count / 2;
                it += step;
                if (m_data[it] < upper) {
                    idx_l = ++it;
                    count -= step + 1;
                } else count = step;
            }
        } else {
            while (idx_l < size() && m_data[idx_l] < upper) idx_l++;
        }
        return VertexSet(m_vid, m_data, idx_l);
    }

    size_t VertexSet::bounded_cnt(IdType upper) const {
        size_t idx_l = 0;
        if (size() > 64) {
            size_t count = size();
            while (count > 0) {
                size_t it = idx_l;
                size_t step = count / 2;
                it += step;
                if (m_data[it] < upper) {
                    idx_l = ++it;
                    count -= step + 1;
                } else count = step;
            }
        } else {
            while (idx_l < size() && m_data[idx_l] < upper) idx_l++;
        }
        return idx_l;
    }

    VertexSet VertexSet::remove(IdType upper) const & {
        size_t idx_l = 0;
        if (size() > 64) {
            size_t count = size();
            while (count > 0) {
                size_t it = idx_l;
                size_t step = count / 2;
                it += step;
                if (m_data[it] < upper) {
                    idx_l = ++it;
                    count -= step + 1;
                } else count = step;
            }
        } else {
            while (idx_l < size() && m_data[idx_l] < upper) idx_l++;
        }

        if (idx_l < m_size && m_data[idx_l] == upper) {
            VertexSet out(size());
            out.m_size = m_size - 1;
            for (size_t i = 0; i < idx_l; i++) {
                out[i] = m_data[i];
            }
            for (size_t i = idx_l; i < m_size - 1; i++) {
                out[i] = m_data[i + 1];
            }
            return out;
        };

        return VertexSet(INVALID_ID, m_data, m_size);
    }

    // A view into an rvalue must retain its workspace after the temporary dies.
    // Lvalue operations intentionally remain borrowed views of their live parent.
    VertexSet VertexSet::bounded(IdType upper) && {
        VertexSet out = static_cast<const VertexSet&>(*this).bounded(upper);
        out.m_pool = m_pool;
        m_pool = nullptr;
        return out;
    }

    VertexSet VertexSet::remove(IdType upper) && {
        VertexSet out = static_cast<const VertexSet&>(*this).remove(upper);
        if (out.m_data == m_data) {
            out.m_pool = m_pool;
            m_pool = nullptr;
        }
        return out;
    }

    size_t VertexSet::remove_cnt(IdType upper) const {
        size_t idx_l = 0;
        if (size() > 64) {
            size_t count = size();
            while (count > 0) {
                size_t it = idx_l;
                size_t step = count / 2;
                it += step;
                if (m_data[it] < upper) {
                    idx_l = ++it;
                    count -= step + 1;
                } else count = step;
            }
        } else {
            while (idx_l < size() && m_data[idx_l] < upper) idx_l++;
        }

        if (idx_l < m_size && m_data[idx_l] == upper) {
            return m_size - 1;
        } else {
            return m_size;
        }
    }

    VertexSet VertexSet::indices(const VertexSet &other) const {
        VertexSet out(size());
        IdType idx_l = 0, idx_r = 0;
        while (idx_l < size() && idx_r < other.size()) {
            const IdType left = m_data[idx_l];
            const IdType right = other[idx_r];
            if (left == right) out[out.m_size++] = idx_l;
            if (left <= right) idx_l++;
            if (right <= left) idx_r++;
        }
        return out;
    }

}
#endif //MINIGRAPH_VERTEX_SET_H
