#include "schedule_heuristics.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <stdexcept>

namespace minigraph {
int supported_iep_width(const std::string &a, int n) {
    int suffix = 1;
    for (int i = n - 2; i >= 0; --i) {
        for (int j = i + 1; j < n; ++j)
            if (a[i * n + j] == '1')
                return suffix >= 3 ? suffix - 1 : 0;
        ++suffix;
    }
    return suffix >= 3 ? suffix - 1 : 0;
}
std::vector<int> outgoing_profile(const std::string &adjacency, int n) {
    std::vector<int> out(n);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            out[i] += adjacency[i * n + j] == '1';
    return out;
}
int bitmap_opportunity_entry(const std::string &a, int n) {
    for (int entry = 0; entry <= n - 4; ++entry)
        for (int anchor = 0; anchor <= entry; ++anchor) {
            bool shared = true;
            for (int v = entry + 1; v < n; ++v)
                shared &= a[anchor * n + v] == '1';
            if (shared)
                return entry;
        }
    return -1;
}
namespace {
std::vector<uint64_t> backward_profile(const std::string &a, int n) {
    std::vector<uint64_t> out(n);
    for (int i = 1; i < n; ++i)
        for (int j = 0; j < i; ++j)
            if (a[i * n + j] == '1')
                out[i] += uint64_t{1} << (n - 1 - j);
    return out;
}
} // namespace
std::vector<ScheduleCandidate> heuristic_candidates(const std::string &a, int n,
                                                    ScheduleHeuristic policy) {
    // Exhaustive experimental search, deliberately bounded rather than silently
    // attempting factorial work for arbitrary user input.
    if (n < 2 || n > 8 || a.size() != static_cast<size_t>(n * n))
        throw std::invalid_argument("Experimental scheduling supports patterns of size 2 through 8");
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::map<std::string, std::vector<int>> unique;
    do {
        bool connected_prefix = true;
        for (int i = 1; i < n && connected_prefix; ++i) {
            bool linked = false;
            for (int j = 0; j < i; ++j)
                linked |= a[order[i] * n + order[j]] == '1';
            connected_prefix = linked;
        }
        if (!connected_prefix)
            continue;
        std::string reordered(n * n, '0');
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                reordered[i * n + j] = a[order[i] * n + order[j]];
        // Permutations arrive lexicographically: retain the smallest labeling.
        unique.emplace(std::move(reordered), order);
    } while (std::next_permutation(order.begin(), order.end()));
    std::vector<ScheduleCandidate> candidates;
    for (auto &[adjacency, permutation] : unique)
        candidates.push_back({std::move(permutation), adjacency});
    if (policy == ScheduleHeuristic::BitmapBalanced || policy == ScheduleHeuristic::Current) {
        std::sort(candidates.begin(), candidates.end(), [n](const auto &l, const auto &r) {
            return backward_profile(l.adjacency, n) > backward_profile(r.adjacency, n);
        });
        candidates.resize(
            std::min(candidates.size(), size_t(policy == ScheduleHeuristic::Current ? 1 : 8)));
    }
    if (candidates.empty() || policy == ScheduleHeuristic::Current)
        return candidates;
    if (policy == ScheduleHeuristic::BitmapBalanced) {
        auto entry_rank = [n](const auto &c) {
            const int entry = bitmap_opportunity_entry(c.adjacency, n);
            return entry < 0 ? n : entry;
        };
        const int baseline = entry_rank(candidates.front());
        const int earliest = entry_rank(
            *std::min_element(candidates.begin(), candidates.end(), [&](const auto &l, const auto &r) {
                return entry_rank(l) < entry_rank(r);
            }));
        // Do not sacrifice the existing heuristic when the shortlist offers no
        // earlier shared universe (including all-array and root-universe cases).
        if (earliest == baseline) {
            candidates.resize(1);
            return candidates;
        }
    }
    auto score = [n, policy](const ScheduleCandidate &candidate) {
        auto result = outgoing_profile(candidate.adjacency, n);
        if (policy == ScheduleHeuristic::IepFirst)
            result.insert(result.begin(), supported_iep_width(candidate.adjacency, n));
        if (policy == ScheduleHeuristic::BitmapBalanced) {
            const int entry = bitmap_opportunity_entry(candidate.adjacency, n);
            result.insert(result.begin(), entry < 0 ? 0 : n - entry);
        }
        return result;
    };
    auto best = score(candidates.front());
    for (const auto &candidate : candidates)
        best = std::max(best, score(candidate));
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const auto &c) { return score(c) != best; }),
                     candidates.end());
    return candidates;
}
} // namespace minigraph
