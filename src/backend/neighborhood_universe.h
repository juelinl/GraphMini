#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace minigraph {
namespace internal {
inline void require_sorted_ids(const uint32_t *ids, size_t size) {
    if (size && !ids)
        throw std::invalid_argument("Null vertex buffer");
    for (size_t i = 1; i < size; ++i)
        if (ids[i - 1] >= ids[i])
            throw std::invalid_argument("Vertex IDs must be sorted and unique");
}
} // namespace internal

// One immutable mapping per anchor binding. Copies share identity, but building
// a new mapping (even with identical vertices/anchor ID) creates a new universe.
// Own the ID snapshot so rebinding or destroying the input graph cannot dangle it.
class NeighborhoodUniverse {
    struct Mapping {
        uint32_t anchor;
        std::vector<uint32_t> ids;
    };
    std::shared_ptr<const Mapping> mapping_;
    const Mapping &mapping() const {
        if (!mapping_)
            throw std::logic_error("Moved-from neighborhood universe");
        return *mapping_;
    }

  public:
    NeighborhoodUniverse(uint32_t anchor, const uint32_t *ids, size_t size) {
        if (size > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Universe exceeds uint32 IDs");
        internal::require_sorted_ids(ids, size);
        auto mapping = std::make_shared<Mapping>();
        mapping->anchor = anchor;
        if (size)
            mapping->ids.assign(ids, ids + size);
        mapping_ = std::move(mapping);
    }
    explicit NeighborhoodUniverse(uint32_t anchor, const std::vector<uint32_t> &ids)
        : NeighborhoodUniverse(anchor, ids.data(), ids.size()) {}
    uint32_t anchor() const { return mapping().anchor; }
    size_t size() const { return mapping().ids.size(); }
    const std::vector<uint32_t> &ids() const { return mapping().ids; }
    uint32_t vertex(size_t position) const { return ids().at(position); }
    size_t lower_bound(uint32_t vertex) const {
        const auto &values = ids();
        return static_cast<size_t>(std::lower_bound(values.begin(), values.end(), vertex) -
                                   values.begin());
    }
    std::optional<size_t> position(uint32_t vertex) const {
        const size_t pos = lower_bound(vertex);
        return pos < size() && ids()[pos] == vertex ? std::optional<size_t>(pos) : std::nullopt;
    }
    bool compatible(const NeighborhoodUniverse &other) const {
        return mapping_ && mapping_ == other.mapping_;
    }
};
} // namespace minigraph
