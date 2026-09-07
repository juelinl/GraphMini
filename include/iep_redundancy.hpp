#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minigraph {

// Count rank assignments on a complete graph, without constructing a graph or
// running a matcher. IEP preserves prefix-to-suffix bounds but drops ordering
// constraints between vertices in the independent suffix. The ratio of relaxed
// to full rank assignments is the multiplicity introduced by that relaxation.
inline long long iep_redundancy(int size, int suffix_size,
                               const std::vector<std::pair<int, int>> &restricts) {
    if (suffix_size <= 1)
        return 1;
    if (size <= 0 || size > 64 || suffix_size > size)
        throw std::invalid_argument("Invalid IEP pattern size");
    std::vector<uint64_t> predecessors(size, 0);
    for (auto [first, second] : restricts) {
        if (first < 0 || second <= first || second >= size)
            throw std::invalid_argument("Invalid symmetry restriction");
        predecessors[second] |= uint64_t{1} << first;
    }
    // Code generation retains transitive bounds from prefix vertices, even if
    // the path that implies a bound passes through another suffix vertex.
    for (int v = 0; v < size; ++v)
        for (int p = 0; p < v; ++p)
            if (predecessors[v] & (uint64_t{1} << p))
                predecessors[v] |= predecessors[p];

    const uint64_t all = size == 64 ? ~uint64_t{0} : (uint64_t{1} << size) - 1;
    auto count = [&](const std::vector<uint64_t> &bounds) {
        std::unordered_map<uint64_t, long long> memo;
        std::function<long long(uint64_t)> visit = [&](uint64_t placed) -> long long {
            if (placed == all)
                return 1;
            if (auto it = memo.find(placed); it != memo.end())
                return it->second;
            long long total = 0;
            for (int v = 0; v < size; ++v) {
                const auto bit = uint64_t{1} << v;
                if (!(placed & bit) && !(bounds[v] & ~placed)) {
                    const auto next = visit(placed | bit);
                    if (next > std::numeric_limits<long long>::max() - total)
                        throw std::overflow_error("IEP rank count overflow");
                    total += next;
                }
            }
            return memo.emplace(placed, total).first->second;
        };
        return visit(0);
    };
    const int prefix_size = size - suffix_size;
    const uint64_t prefix = (uint64_t{1} << prefix_size) - 1;
    auto relaxed_bounds = predecessors;
    for (int v = prefix_size; v < size; ++v)
        relaxed_bounds[v] &= prefix;
    if (relaxed_bounds == predecessors)
        return 1;
    const auto full = count(predecessors);
    const auto relaxed = count(relaxed_bounds);
    if (full == 0 || relaxed % full != 0)
        throw std::runtime_error("Non-integral IEP symmetry correction");
    const auto factor = relaxed / full;
    if (factor > std::numeric_limits<int>::max())
        throw std::overflow_error("IEP correction exceeds runtime counter divisor");
    return factor;
}

} // namespace minigraph
