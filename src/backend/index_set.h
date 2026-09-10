#pragma once
#include "vertex_set_pool.h"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace minigraph {
// Move-only lease for decoded LOCAL indices, not global vertex IDs. Elements
// are uninitialized until the decoder writes them; size records the written
// prefix. The owner must stay on its originating worker. Other workers may
// borrow read-only slices until the synchronous join, never ownership.
class IndexSet {
  internal::VertexSetPool *pool_{nullptr};
  uint32_t *data_{nullptr};
  size_t size_{0};

  void release() noexcept {
    if (pool_)
      pool_->release(data_);
  }

public:
  IndexSet() = default;
  explicit IndexSet(size_t capacity) {
    if (!capacity)
      return;
    pool_ = &internal::VertexSetPool::for_request(capacity);
    data_ = pool_->acquire();
  }
  ~IndexSet() { release(); }
  IndexSet(const IndexSet &) = delete;
  IndexSet &operator=(const IndexSet &) = delete;
  IndexSet(IndexSet &&other) noexcept
      : pool_(std::exchange(other.pool_, nullptr)),
        data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)) {}
  IndexSet &operator=(IndexSet &&other) noexcept {
    if (this != &other) {
      release();
      pool_ = std::exchange(other.pool_, nullptr);
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
  uint32_t *data() & noexcept { return data_; }
  const uint32_t *data() const & noexcept { return data_; }
  uint32_t *data() && = delete;
  const uint32_t *data() const && = delete;
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return pool_ ? pool_->capacity() : 0; }
  void set_size(size_t size) {
    if (size > capacity())
      throw std::length_error("IndexSet size exceeds capacity");
    size_ = size;
  }
};
} // namespace minigraph
