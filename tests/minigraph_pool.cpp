#ifdef GRAPHMINI_POOL_STANDALONE
#include "backend/managed_container.h"
#elif defined(GRAPHMINI_PROFILE_RUNTIME)
#include "backend_prof/minigraph.h"
#else
#include "backend/minigraph.h"
#endif
#include <iostream>
#include <type_traits>

using namespace minigraph;
static_assert(!std::is_copy_constructible_v<MiniGraphPool>);
static_assert(!std::is_copy_constructible_v<ManagedContainer>);
static_assert(std::is_nothrow_move_constructible_v<ManagedContainer>);
void require(bool ok) { if (!ok) throw std::runtime_error("MiniGraph pool regression"); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::length_error&) { rejected = true; }
    require(rejected);
}
#ifndef GRAPHMINI_POOL_STANDALONE
template<class MiniGraph> void check_graph_rebuilds() {
    for (bool bounded : {false, true}) {
        MiniGraph parent(bounded), child(bounded);
        for (size_t n : {32, 128, 8, 256, 16}) {
            Graph graph;
            graph.num_vertex = n;
            graph.num_edge = n * (n - 1);
            graph.m_indptr = new uint64_t[n + 1];
            graph.m_offset = new uint64_t[n];
            graph.m_indices = new IdType[graph.num_edge];
            std::vector<IdType> all(n), even;
            size_t pos = 0;
            for (size_t i = 0; i < n; ++i) {
                all[i] = i;
                if (i % 2 == 0) even.push_back(i);
                graph.m_indptr[i] = pos;
                graph.m_offset[i] = i;
                for (size_t j = 0; j < n; ++j) if (i != j) graph.m_indices[pos++] = j;
            }
            graph.m_indptr[n] = pos;
            MiniGraphIF::DATA_GRAPH = &graph;
            VertexSet vertices(0, all.data(), all.size()), subset(0, even.data(), even.size());
            parent.build(vertices, vertices, vertices);
            child.build(&parent, subset, subset, subset);
            auto check = [&](MiniGraph& mg, const std::vector<IdType>& ids) {
                for (size_t i = 0; i < ids.size(); ++i) {
                    auto neighbors = mg.N(i); // borrowed adjacency, inspected while owner is live
                    size_t k = 0;
                    for (auto id : ids) if (id != ids[i] && (!bounded || id < ids[i])) {
                        require(k < neighbors.size() && neighbors[k++] == id);
                    }
                    require(k == neighbors.size());
                }
            };
            check(parent, all);
            check(child, even);
        }
    }
    MiniGraphIF::DATA_GRAPH = nullptr;
}
#endif
int main() {
    {
        MiniGraphPool pool;
        MiniGraphPool::TOTAL_ALLOCATED = 0;
        auto small = pool.AllocateWorkSpace(0);
        require(small.capacity == 1024 && pool.checked_out() == 1);
        pool.FreeWorkSpace(small);
        auto reused = pool.AllocateWorkSpace(1024);
        require(reused.data == small.data && MiniGraphPool::TOTAL_ALLOCATED == 4096);
        for (size_t i = 0; i < reused.capacity; ++i) reused[i] = i;
        pool.Resize(reused, 1); // shrinking must not allocate or copy out of bounds
        require(reused.data == small.data);
        pool.Resize(reused, 2049);
        for (size_t i = 0; i < 1024; ++i) require(reused[i] == i);
        pool.FreeWorkSpace(reused);
        auto a = pool.AllocateWorkSpace(5000), b = pool.AllocateWorkSpace(12000);
        pool.FreeWorkSpace(b);
        pool.FreeWorkSpace(a);
        MiniGraphPool::TOTAL_ALLOCATED = 0;
        auto best = pool.AllocateWorkSpace(5000);
        require(best.data == a.data && MiniGraphPool::TOTAL_ALLOCATED == 0);
        auto* before = best.data;
        rejects([&] { pool.Resize(best, std::numeric_limits<size_t>::max()); });
        require(best.data == before);
        pool.FreeWorkSpace(best);
        require(pool.checked_out() == 0);
    }
    auto& pool = MiniGraphPool::Get();
    {
        ManagedContainer empty;
        require(empty.begin() == nullptr && empty.end() == nullptr);
        rejects([&] { empty.set_size(1); });
        ManagedContainer data(8);
        // Scratch writes past logical size must survive growth, too.
        for (size_t i = 0; i < data.capacity(); ++i) data[i] = i + 17;
        auto* borrowed = data.begin();
        ManagedContainer moved(std::move(data));
        require(moved.begin() == borrowed && data.begin() == nullptr);
        moved.Resize(4097);
        require(moved.size() == 8);
        for (size_t i = 0; i < 1024; ++i) require(moved[i] == i + 17);
        auto* before = moved.begin();
        const size_t cap = moved.capacity();
        moved.Reserve(1);
        require(moved.begin() == before);
        rejects([&] { moved.Reserve(std::numeric_limits<size_t>::max()); });
        require(moved.begin() == before && moved.capacity() == cap && moved[7] == 24);
        rejects([&] { moved.set_size(cap + 1); });
        require(moved.size() == 8);
        moved.Reserve(cap + 1); // discard on growth; do not read discarded values
        require(moved.size() == 8 && moved.capacity() > cap);
        ManagedContainer target(4);
        target = std::move(moved);
        require(target.size() == 8 && moved.size() == 4); // swap-based assignment
        target = std::move(target);
        require(target.size() == 8);
        empty.Resize(17);
        empty.set_size(17);
        for (size_t n : {9000, 3, 18000, 64, 25000}) {
            empty.Resize(n);
            empty.set_size(n);
            for (size_t i = 0; i < n; ++i) empty[i] = i;
            require(empty[n - 1] == n - 1);
        }
    }
    require(pool.checked_out() == 0);
    auto worker = [] {
        for (size_t n : {1, 1024, 1025, 16384, 3, 24000}) {
            ManagedContainer data(n);
            data[n - 1] = 42;
            data.Resize(data.capacity() + 1);
            require(data[n - 1] == 42);
        }
        require(MiniGraphPool::Get().checked_out() == 0);
    };
    std::thread a(worker), b(worker);
    a.join(); b.join();
#ifndef GRAPHMINI_POOL_STANDALONE
#ifdef GRAPHMINI_PROFILE_RUNTIME
    VertexSet::profiler = std::make_shared<Profiler>(8, 256);
#endif
    check_graph_rebuilds<MiniGraphEager>();
    check_graph_rebuilds<MiniGraphLazy>();
    check_graph_rebuilds<MiniGraphOnline>();
    check_graph_rebuilds<MiniGraphCostModel>();
    require(pool.checked_out() == 0);
#endif
    std::cout << "MiniGraph pool reuse, growth, moves, bounds, accounting and workers passed\n";
}
