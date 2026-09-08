#include "backend/bitmap_count_region.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>

// Single-threaded allocation audit; no framework work occurs while enabled.
static bool track = false;
static size_t allocations = 0;
void *operator new(std::size_t size) {
    if (track)
        ++allocations;
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

int main() {
    using namespace minigraph;
    struct Graph {
        std::vector<uint32_t> ids;
        const std::vector<uint32_t> &N(uint32_t) const { return ids; }
    } graph;
    graph.ids.resize(257);
    std::iota(graph.ids.begin(), graph.ids.end(), 100);
    auto region = BitmapCountRegion::build(graph, 99, graph.ids, graph.ids, 1);
    const auto view = region->input_view(0);
    const auto *words = view.data();
    std::vector<uint32_t> small(graph.ids.begin(), graph.ids.begin() + 65);
    const std::vector<uint32_t> *inputs[] = {&small};
    track = true;
    for (int i = 0; i < 100; ++i) {
        inputs[0] = i % 2 ? &small : &graph.ids;
        region->bind_inputs(inputs, 1);
        if (view.count() != inputs[0]->size() || region->input_view(0).data() != words)
            std::abort();
        size_t visited = 0;
        for (auto cursor = region->local_cursor(0); cursor.valid(); cursor.advance())
            ++visited;
        if (visited != view.count())
            std::abort();
    }
    track = false;
    if (allocations)
        throw std::logic_error("Live-in rebinding/local iteration allocated memory");
    NeighborhoodUniverse universe(99, graph.ids);
    size_t first = 0;
    for (size_t rows : {size_t{10}, graph.ids.size()}) {
        std::vector<uint32_t> ids(graph.ids.begin(), graph.ids.begin() + rows);
        allocations = 0;
        track = true;
        BitGraph bits(universe, std::move(ids), [&](uint32_t v) -> const auto & { return graph.N(v); });
        track = false;
        if (rows == 10)
            first = allocations;
        else if (allocations != first)
            throw std::logic_error("Scratch allocations increased with row count");
    }
    std::cout << "Zero allocations in 100 rebindings/local traversals; " << first
              << " row-storage/scratch allocations independent of row count\n";
}
