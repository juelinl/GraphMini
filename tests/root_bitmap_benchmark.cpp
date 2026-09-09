// Single-thread per-root diagnostic harness, using production set/bitmap ops.
// Fixed clique kernels isolate reuse; this is not the generated-query runner.
#include "backend/bitmap_count_region.h"
#include "backend/set_ops/set_ops.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace minigraph;
using Clock = std::chrono::steady_clock;
double ns(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now()-start).count();
}
struct Graph {
    std::vector<std::vector<uint32_t>> rows;
    const std::vector<uint32_t> &N(uint32_t v) const { return rows.at(v); }
};
struct Trace { uint64_t uses = 0; };
template<int Left, bool Instrument>
uint64_t arrays(const Graph &g, const uint32_t *ids, size_t size,
                std::vector<std::vector<uint32_t>> &scratch, Trace &trace) {
    uint64_t total = 0;
    for (size_t i = 0; i < size; ++i) {
        const auto &row = g.N(ids[i]);
        if constexpr (Instrument) ++trace.uses;
        if constexpr (Left == 2)
            total += set_ops::intersection_count(ids, i, row.data(), row.size());
        else {
            auto &out = scratch[Left];
            const auto n = set_ops::intersection_write(ids, i, row.data(), row.size(), out.data());
            total += arrays<Left-1, Instrument>(g, out.data(), n, scratch, trace);
        }
    }
    return total;
}
template<int Left, size_t Words, bool Instrument>
uint64_t bitmaps(BitmapCountRegion &region, Trace &trace) {
    uint64_t total = 0;
    for (auto cursor = region.local_cursor(Left); cursor.valid(); cursor.advance()) {
        const auto pos = cursor.position();
        if constexpr (Instrument) ++trace.uses;
        if constexpr (Left == 2) total += region.count_local<Words>(Left, pos, false, true);
        else {
            region.materialize_local<Words>(Left-1, Left, pos, false, true);
            total += bitmaps<Left-1, Words, Instrument>(region, trace);
        }
    }
    return total;
}
template<int K, bool Instrument>
uint64_t bitmap_root(const Graph &g, uint32_t root, const std::vector<uint32_t> &prefix,
                     Trace &trace, double *build = nullptr, double *bind = nullptr, double *execute = nullptr) {
    Clock::time_point t;
    if constexpr (Instrument) t = Clock::now();
    const auto &neighbors = g.N(root);
    auto region = BitmapCountRegion::build(g, root, neighbors, neighbors, K);
    if (build) *build = ns(t);
    if (!region) {
        if (!neighbors.empty()) throw std::runtime_error("Fixture exceeded bitmap budget");
        return 0;
    }
    if constexpr (Instrument) t = Clock::now();
    region->bind_input(K-1, prefix);
    if (bind) *bind = ns(t);
    if constexpr (Instrument) t = Clock::now();
    const auto count = neighbors.size() <= 64 ? bitmaps<K-1, 1, Instrument>(*region, trace)
                     : neighbors.size() <= 128 ? bitmaps<K-1, 2, Instrument>(*region, trace)
                     : bitmaps<K-1, 0, Instrument>(*region, trace);
    if (execute) *execute = ns(t);
    return count;
}
struct Stats { uint64_t triangles; double exact_ns, sample_ns, density; };
uint64_t triangles(const Graph &g, uint32_t root) {
    const auto &a = g.N(root);
    uint64_t twice = 0;
    for (auto v : a) {
        const auto &b = g.N(v);
        twice += set_ops::intersection_count(a.data(), a.size(), b.data(), b.size());
    }
    return twice/2;
}
double sample_density(const Graph &g, uint32_t root) {
    const auto &a = g.N(root);
    uint64_t state = 20260909 + root;
    auto random = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    size_t hits = 0;
    if (a.size() > 1)
        for (size_t i = 0; i < 32; ++i) {
            auto x = random() % a.size(), y = random() % (a.size()-1);
            if (y >= x) ++y;
            const auto &row = g.N(a[x]);
            hits += std::binary_search(row.begin(), row.end(), a[y]);
        }
    return hits/32.0;
}
Stats stats(const Graph &g, uint32_t root) {
    auto t = Clock::now(); const auto count = triangles(g, root); const auto exact = ns(t);
    t = Clock::now(); const auto density = sample_density(g, root); const auto sample = ns(t);
    return {count, exact, sample, density};
}
volatile uint64_t sink = 0;
template<int K>
void run(const Graph &g, const std::vector<uint32_t> &roots, size_t max_degree) {
    std::vector<std::vector<uint32_t>> scratch(K+1, std::vector<uint32_t>(max_degree));
    for (auto root : roots) {
        const auto &neighbors = g.N(root);
        const auto end = std::lower_bound(neighbors.begin(), neighbors.end(), root);
        std::vector<uint32_t> prefix(neighbors.begin(), end);
        const auto features = stats(g, root);
        Trace at, bt;
        auto expected = arrays<K-1, true>(g, prefix.data(), prefix.size(), scratch, at);
        double build = 0, bind = 0, execute = 0;
        auto actual = bitmap_root<K, true>(g, root, prefix, bt, &build, &bind, &execute);
        if (actual != expected || at.uses != bt.uses) throw std::runtime_error("Root parity failure");
        double initial[2] = {};
        auto work = [&](int bitmap) {
            Trace ignored;
            return bitmap ? bitmap_root<K, false>(g, root, prefix, ignored)
                          : arrays<K-1, false>(g, prefix.data(), prefix.size(), scratch, ignored);
        };
        for (int b = 0; b < 2; ++b) {
            auto t = Clock::now(); sink = work(b); initial[b] = ns(t);
        }
        const size_t repeats = std::clamp<size_t>(200000.0 / std::max({initial[0], initial[1], 1.0}), 1, 128);
        std::vector<double> timing[2];
        for (int trial = 0; trial < 5; ++trial)
            for (int order = 0; order < 2; ++order) {
                const int b = (trial + order + root) % 2;
                auto t = Clock::now(); uint64_t result = 0;
                for (size_t rep = 0; rep < repeats; ++rep) result += work(b);
                sink = result;
                timing[b].push_back(ns(t)/repeats);
            }
        for (auto &values : timing) std::sort(values.begin(), values.end());
        std::cout << "{\"root\":" << root << ",\"k\":" << K << ",\"degree\":" << neighbors.size()
                  << ",\"prefix\":" << prefix.size() << ",\"triangles\":" << features.triangles
                  << ",\"sample_density\":" << features.density << ",\"exact_ns\":" << features.exact_ns
                  << ",\"sample_ns\":" << features.sample_ns << ",\"uses\":" << at.uses
                  << ",\"matches\":" << expected << ",\"array_ns\":" << timing[0][2]
                  << ",\"bitmap_ns\":" << timing[1][2] << ",\"build_ns\":" << build
                  << ",\"bind_ns\":" << bind << ",\"execute_ns\":" << execute << "}\n" << std::flush;
    }
}
// End-to-end selector check in the same harness, including feature extraction.
// Policies are supplied after being frozen on synthetic data.
struct Policy {
    std::string family;
    double minimum = 0, parameter = 0;
    explicit Policy(std::string text) {
        const auto first = text.find(':');
        family = text.substr(0, first);
        if (first != std::string::npos) {
            const auto second = text.find(':', first+1);
            minimum = std::stod(text.substr(first+1, second-first-1));
            if (second != std::string::npos) parameter = std::stod(text.substr(second+1));
        }
        if (family != "probe" && family != "pilot" && family != "array" && family != "bitmap" && family != "degree" && family != "prefix" &&
            family != "sample" && family != "sample_work" && family != "triangle" && family != "triangle_work")
            throw std::runtime_error("Unsupported timed policy");
        if ((family == "pilot" || family == "probe") &&
            (minimum != 2 && minimum != 4))
            throw std::runtime_error("Pilot/probe count must be 2 or 4");
    }
    bool select(const Graph &g, uint32_t root, size_t prefix, int k) const {
        const size_t d = g.N(root).size();
        if (family == "array") return false;
        if (family == "bitmap") return true;
        if (family == "degree") return d >= minimum;
        if (prefix < minimum) return false;
        if (family == "prefix") return double(prefix)/std::max<size_t>(d,1) >= parameter;
        const double density = family.find("sample") == 0 ? sample_density(g, root)
            : 2.0*triangles(g,root)/std::max<double>(double(d)*(double(d)-1),1);
        if (family.find("_work") == std::string::npos) return density >= parameter;
        double uses = 0, combinations = 1;
        for (int depth = 1; depth < k-1 && depth <= static_cast<int>(prefix); ++depth) {
            combinations *= double(prefix-depth+1)/depth;
            uses += combinations * std::pow(density, depth*(depth-1)/2);
        }
        return uses/std::max<size_t>(d,1) >= parameter;
    }
};
// Complete the largest-ID child branches with arrays. Their intersections are
// useful query work, not a separate feature pass. Remaining children only use
// smaller IDs, so truncating the prefix prevents recounting completed branches
// without removing any candidate they can legally match.
template<int K>
uint64_t pilot_root(const Graph &g, uint32_t root, std::vector<uint32_t> &prefix,
                    std::vector<std::vector<uint32_t>> &scratch,
                    const Policy &policy, bool &selected) {
    Trace ignored;
    const size_t original = prefix.size();
    const size_t pilots = std::min(original, static_cast<size_t>(policy.minimum));
    uint64_t count = 0, children = 0;
    for (size_t j = 0; j < pilots; ++j) {
        const size_t i = original-1-j;
        const auto &row = g.N(prefix[i]);
        auto &out = scratch[K-1];
        const auto size = set_ops::intersection_write(prefix.data(), i, row.data(), row.size(), out.data());
        children += size;
        count += arrays<K-2,false>(g,out.data(),size,scratch,ignored);
    }
    prefix.resize(original-pilots);
    // A deliberately simple proxy for remaining second-level branching relative
    // to universe construction size; thresholds are fitted on synthetic data.
    const double score = double(children)*prefix.size()/
        std::max<double>(double(pilots)*g.N(root).size(),1);
    selected = prefix.size() >= K-1 && children && score >= policy.parameter;
    return count + (selected ? bitmap_root<K,false>(g,root,prefix,ignored)
        : arrays<K-1,false>(g,prefix.data(),prefix.size(),scratch,ignored));
}
template<int K>
uint64_t probe_root(const Graph &g, uint32_t root, std::vector<uint32_t> &prefix,
                    std::vector<std::vector<uint32_t>> &scratch,
                    std::vector<std::vector<uint32_t>> &saved,
                    const Policy &policy, bool &selected) {
    Trace ignored;
    const size_t original = prefix.size();
    const size_t probes = std::min(original, saved.size());
    uint64_t children = 0, count = 0;
    for (size_t j = 0; j < probes; ++j) {
        const size_t i = original-1-j;
        const auto &row = g.N(prefix[i]);
        saved[j].resize(g.N(root).size());
        const auto size = set_ops::intersection_write(prefix.data(), i, row.data(), row.size(), saved[j].data());
        saved[j].resize(size);
        children += size;
    }
    prefix.resize(original-probes);
    const double score = double(children)*prefix.size()/
        std::max<double>(double(probes)*g.N(root).size(),1);
    selected = prefix.size() >= K-1 && children && score >= policy.parameter;
    if (!selected) {
        for (size_t j = 0; j < probes; ++j)
            count += arrays<K-2,false>(g,saved[j].data(),saved[j].size(),scratch,ignored);
        return count + arrays<K-1,false>(g,prefix.data(),prefix.size(),scratch,ignored);
    }
    const auto &neighbors = g.N(root);
    auto region = BitmapCountRegion::build(g,root,neighbors,neighbors,K);
    if (!region) throw std::runtime_error("Probe fixture exceeded bitmap budget");
    auto execute = [&](auto words) {
        constexpr size_t Words = decltype(words)::value;
        uint64_t result = 0;
        for (size_t j = 0; j < probes; ++j) {
            region->bind_input(K-2,saved[j]);
            result += bitmaps<K-2,Words,false>(*region,ignored);
        }
        region->bind_input(K-1,prefix);
        return result + bitmaps<K-1,Words,false>(*region,ignored);
    };
    return neighbors.size() <= 64 ? execute(std::integral_constant<size_t,1>{})
         : neighbors.size() <= 128 ? execute(std::integral_constant<size_t,2>{})
         : execute(std::integral_constant<size_t,0>{});
}
template<int K>
void timed_policy(const Graph &g, const std::vector<uint32_t> &roots, size_t max_degree, const Policy &policy) {
    std::vector<std::vector<uint32_t>> scratch(K+1, std::vector<uint32_t>(max_degree));
    std::vector<uint32_t> prefix; prefix.reserve(max_degree);
    std::vector<std::vector<uint32_t>> saved(policy.family == "probe" ? static_cast<size_t>(policy.minimum) : 0);
    for (auto &row : saved) row.reserve(max_degree);
    std::vector<double> samples;
    uint64_t expected = 0; size_t selected = 0;
    for (int trial = 0; trial < 6; ++trial) {
        auto start = Clock::now();
        uint64_t count = 0; selected = 0;
        for (auto root : roots) {
            const auto &neighbors = g.N(root);
            prefix.assign(neighbors.begin(), std::lower_bound(neighbors.begin(), neighbors.end(), root));
            Trace ignored;
            if (policy.family == "pilot" || policy.family == "probe") {
                bool use = false;
                count += policy.family == "pilot" ? pilot_root<K>(g,root,prefix,scratch,policy,use)
                    : probe_root<K>(g,root,prefix,scratch,saved,policy,use);
                selected += use;
                continue;
            }
            const bool use = policy.select(g, root, prefix.size(), K); selected += use;
            count += use ? bitmap_root<K,false>(g,root,prefix,ignored)
                         : arrays<K-1,false>(g,prefix.data(),prefix.size(),scratch,ignored);
        }
        const auto elapsed = ns(start);
        if (!trial) expected = count;
        else { if (count != expected) throw std::runtime_error("Unstable policy count"); samples.push_back(elapsed); }
        sink = count;
    }
    std::sort(samples.begin(),samples.end());
    std::cout << "{\"k\":" << K << ",\"policy\":\"" << policy.family << "\",\"matches\":" << expected
              << ",\"roots\":" << roots.size() << ",\"selected\":" << selected
              << ",\"total_ns\":" << samples[2] << "}\n" << std::flush;
}
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) throw std::runtime_error("usage: root_bitmap_benchmark graph.txt root_limit [policy]");
    std::ifstream file(argv[1]);
    size_t n, m;
    if (!(file >> n >> m)) throw std::runtime_error("Invalid graph");
    Graph g; g.rows.resize(n);
    for (size_t i = 0; i < m; ++i) {
        uint32_t a, b; if (!(file >> a >> b) || a >= n || b >= n || a == b) throw std::runtime_error("Invalid edge");
        g.rows[a].push_back(b); g.rows[b].push_back(a);
    }
    size_t max_degree = 0;
    for (auto &row : g.rows) {
        std::sort(row.begin(), row.end()); row.erase(std::unique(row.begin(), row.end()), row.end());
        max_degree = std::max(max_degree, row.size());
    }
    // Degree-stratified sample, including extreme roots; no wiki tuning.
    std::vector<uint32_t> order(n); std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return g.N(a).size() < g.N(b).size(); });
    const size_t limit = std::min(n, static_cast<size_t>(std::stoul(argv[2])));
    std::set<uint32_t> selected;
    for (size_t i = 0; i < limit; ++i) selected.insert(order[limit == 1 ? 0 : i*(n-1)/(limit-1)]);
    std::vector<uint32_t> roots(selected.begin(), selected.end());
    if (argc == 4) {
        Policy policy(argv[3]);
        timed_policy<6>(g,roots,max_degree,policy); timed_policy<7>(g,roots,max_degree,policy);
    } else { run<6>(g, roots, max_degree); run<7>(g, roots, max_degree); }
}
