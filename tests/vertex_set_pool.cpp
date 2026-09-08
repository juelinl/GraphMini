#ifdef GRAPHMINI_PROFILE_RUNTIME
#include "backend_prof/vertex_set.h"
#else
#include "backend/vertex_set.h"
#endif
#include <iostream>
#include <thread>

using namespace minigraph;
static_assert(sizeof(void*) != 8 || sizeof(VertexSet) == 24,
              "VertexSet should contain two pointers and two 32-bit fields");
void require(bool b) { if (!b) throw std::runtime_error("VertexSetPool regression"); }
void fill(VertexSet& set, size_t n, uint32_t offset = 0) {
    set.set_size(n);
    for (size_t i = 0; i < n; ++i) set[i] = offset + i;
}
int main() {
    const uint64_t max_size = std::numeric_limits<IdType>::max();
    // Metadata-only boundary checks: never dereference these synthetic views.
    VertexSet boundary(0, nullptr, max_size);
    require(boundary.size() == max_size);
    bool overflow_rejected = false;
    try { VertexSet invalid(0, nullptr, max_size + 1); }
    catch (const std::length_error&) { overflow_rejected = true; }
    require(overflow_rejected);
    if constexpr (sizeof(size_t) > sizeof(IdType)) {
        overflow_rejected = false;
        try { boundary.set_size(static_cast<size_t>(max_size + 1)); }
        catch (const std::length_error&) { overflow_rejected = true; }
        require(overflow_rejected && boundary.size() == max_size);
        overflow_rejected = false;
        try { VertexSet invalid(static_cast<size_t>(max_size + 1)); }
        catch (const std::length_error&) { overflow_rejected = true; }
        require(overflow_rejected);
    }
    internal::VertexSetPool::configure_for_graph(3);
    overflow_rejected = false;
    try { internal::VertexSetPool::configure_for_graph(max_size + 1); }
    catch (const std::length_error&) { overflow_rejected = true; }
    require(overflow_rejected);
    // Invalid configuration must leave the previous default intact.
    internal::VertexSetPool::TOTAL_ALLOCATED = 0;
    {
        VertexSet set(0);
        fill(set, 4);
        require(internal::VertexSetPool::TOTAL_ALLOCATED == 4 * sizeof(IdType));
    }
    internal::VertexSetPool::TOTAL_ALLOCATED = 0;
    { VertexSet reused(0); }
    require(internal::VertexSetPool::TOTAL_ALLOCATED == 0);
    internal::VertexSetPool::configure_for_graph(0);
    std::atomic_uint64_t allocated{0};
    {
        internal::VertexSetPool pool(17, allocated);
        auto* p = pool.acquire();
        require(pool.capacity() == 17 && pool.checked_out() == 1);
        for (size_t i = 0; i < 17; ++i) p[i] = i;
        pool.release(p);
        auto* reused = pool.acquire();
        require(reused == p && allocated == 17 * sizeof(uint32_t));
        pool.release(reused);
        require(pool.buffer_count() == 1 && pool.checked_out() == 0);
    }
    bool rejected = false;
    try { internal::VertexSetPool invalid(0, allocated); }
    catch (const std::length_error&) { rejected = true; }
    require(rejected);

    internal::VertexSetPool::configure_for_graph(3);
    VertexSet old(3);
    fill(old, 3, 10);
    {
        VertexSet churn(3);
        fill(churn, 3);
    }
    // Grow while an old owner is still live, then release owners in mixed order.
    internal::VertexSetPool::configure_for_graph(255);
    {
        VertexSet large(255);
        fill(large, 255);
        require(old[0] == 10 && old[2] == 12);
        auto moved = std::move(large).bounded(100).remove(999);
        require(moved.pooled() && moved.size() == 100 && moved[99] == 99);
        VertexSet copy = moved;
        require(!copy.pooled());
        VertexSet destination(255);
        destination = std::move(moved);
        require(destination.pooled() && destination[99] == 99);
        VertexSet assigned;
        assigned = destination;
        require(!assigned.pooled() && assigned[99] == 99);
    }
    internal::VertexSetPool::configure_for_graph(1);
    {
        VertexSet small(1);
        fill(small, 1, 7);
        // The explicit constructor request cannot silently exceed allocation.
        VertexSet standalone(1024);
        fill(standalone, 1024);
        require(standalone[1023] == 1023 && old[2] == 12);
    }
    for (size_t degree : {2, 2048, 4, 4096, 8, 2048}) {
        internal::VertexSetPool::configure_for_graph(degree);
        VertexSet set(degree);
        fill(set, degree);
        require(set[degree - 1] == degree - 1 && old[0] == 10);
    }
    // Independent workers own independent pools. Owners are never transferred
    // between threads; all buffers are returned before each worker exits.
    auto worker = [] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            VertexSet set(4096);
            fill(set, 4096);
            require(set[4095] == 4095);
        }
    };
    std::thread a(worker), b(worker);
    a.join(); b.join();
    std::cout << "VertexSet size: " << sizeof(VertexSet)
              << " bytes; size boundaries, pool reuse, growth/shrink, moves/views and workers passed\n";
}
