#include "backend/backend.h"

// Deliberately compiled as C++17, like the Python host.
void backend_host_configure_pool() {
    minigraph::internal::VertexSetPool::configure_for_graph(15);
    minigraph::internal::VertexSetPool::TOTAL_ALLOCATED = 0;
}

bool backend_host_check(const minigraph::Graph& graph, const minigraph::Context& context) {
    return graph.num_vertex == 7 && context.per_thread_result.at(0).count == 42;
}

bool backend_host_check_set(const minigraph::VertexSet& set) {
    auto borrowed = set;
    return set.pooled() && !borrowed.pooled() && borrowed.size() == 8 && borrowed[7] == 7 &&
           minigraph::internal::VertexSetPool::TOTAL_ALLOCATED == 16 * sizeof(minigraph::IdType);
}
