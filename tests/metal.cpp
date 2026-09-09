#include "backend/metal/backend.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace minigraph::metal;
static void edge(Adjacency &g, unsigned a, unsigned b) { g[a].push_back(b); g[b].push_back(a); }
static void sort(Adjacency &g) { for (auto &r : g) std::sort(r.begin(),r.end()); }
// Independent combination oracle: no production set/bitmap operations.
static uint64_t oracle(const Adjacency &g, unsigned k) {
    std::vector<unsigned> chosen;
    std::function<uint64_t(unsigned)> visit = [&](unsigned start) {
        if (chosen.size() == k) return uint64_t{1};
        uint64_t sum = 0;
        for (unsigned v = start; v < g.size(); ++v) {
            if (g.size()-v < k-chosen.size()) break;
            if (!std::all_of(chosen.begin(),chosen.end(),[&](auto u) {
                    return std::binary_search(g[v].begin(),g[v].end(),u); })) continue;
            chosen.push_back(v); sum += visit(v+1); chosen.pop_back();
        }
        return sum;
    };
    return visit(0);
}
int main() {
    try {
        Backend backend;
        unsigned checks = 0;
        auto check = [&](const Adjacency &g, unsigned k, size_t tasks = 16384) {
            Options o; o.clique_size = k; o.max_tasks = tasks;
            const auto expected = oracle(g,k), actual = backend.count_cliques(g,o).matches;
            if (actual != expected) throw std::runtime_error("Metal/oracle mismatch");
            ++checks;
        };
        for (unsigned seed = 0; seed < 12; ++seed) {
            std::mt19937 rng(seed);
            Adjacency g(11);
            for (unsigned a = 0; a < g.size(); ++a)
                for (unsigned b = a+1; b < g.size(); ++b)
                    if (rng()%100 < 30+seed*6) edge(g,a,b);
            sort(g);
            for (unsigned k = 3; k <= 8; ++k) check(g,k,seed%2 ? 1 : 16384);
        }
        // Neighborhood word boundaries, including high local-ID set bits.
        for (unsigned d : {1,63,64,65,127,128,129}) {
            Adjacency g(d+1);
            for (unsigned v = 0; v < d; ++v) edge(g,v,d);
            for (unsigned a = d > 7 ? d-7 : 0; a < d; ++a)
                for (unsigned b = a+1; b < d; ++b) edge(g,a,b);
            sort(g);
            for (unsigned k : {3,6,7,8}) check(g,k);
        }
        check({},6); check(Adjacency(10),7);
        // Vertex-ID permutation changes canonical prefixes but not counts.
        Adjacency g(12);
        for (unsigned a = 0; a < 8; ++a)
            for (unsigned b = a+1; b < 8; ++b) edge(g,(a*5)%12,(b*5)%12);
        sort(g); check(g,6,2); check(g,7,2);
        auto rejects = [&](const Adjacency &graph, Options o) {
            try { backend.count_cliques(graph,o); }
            catch (const std::exception &) { ++checks; return; }
            throw std::runtime_error("Invalid Metal input accepted");
        };
        rejects({{1},{}},{}); rejects({{0}},{});
        Options bad; bad.clique_size = 9; rejects({},bad);
        bad = {}; bad.max_tasks = 0; rejects({},bad);
        Adjacency large(128);
        for (unsigned a = 0; a < large.size(); ++a)
            for (unsigned b = a+1; b < large.size(); ++b) edge(large,a,b);
        sort(large);
        bad = {}; bad.batch_bytes = 1024; rejects(large,bad);
        // More than 2^32 total matches, without an expensive enumeration oracle.
        uint64_t choose = 1;
        for (uint64_t i = 1; i <= 6; ++i) choose = choose*(129-i)/i;
        if (choose <= UINT32_MAX || backend.count_cliques(large).matches != choose)
            throw std::runtime_error("64-bit clique count mismatch");
        ++checks;
        std::cout << checks << " Metal correctness/error checks passed on " << backend.device_name() << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
