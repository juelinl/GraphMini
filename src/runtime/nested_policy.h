#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace minigraph {
// Evaluate once at query entry, using the graph being searched, not the graph
// (if any) that was present when the kernel was compiled.
inline size_t nested_threshold(uint64_t vertices, uint64_t edges,
                               uint64_t max_degree, size_t factor) {
    const uint64_t average = vertices ? edges / vertices : 0;
    const auto limit = std::numeric_limits<size_t>::max();
    const size_t threshold = factor && average > limit / factor
        ? limit : static_cast<size_t>(average) * factor;
    return average && max_degree / average > 100
        ? std::min(threshold, size_t{100}) : threshold;
}
} // namespace minigraph
