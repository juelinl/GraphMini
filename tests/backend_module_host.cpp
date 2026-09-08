#include "backend/backend.h"

// Deliberately compiled as C++17, like the Python host.
bool backend_host_check(const minigraph::Graph& graph, const minigraph::Context& context) {
    return graph.num_vertex == 7 && context.per_thread_result.at(0).count == 42;
}

bool backend_host_check_set(const minigraph::VertexSet& set) {
    auto borrowed = set;
    return set.pooled() && !borrowed.pooled() && borrowed.size() == 8 && borrowed[7] == 7;
}
