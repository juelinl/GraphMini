import graphmini.backend;

bool backend_host_check(const minigraph::Graph&, const minigraph::Context&);
bool backend_host_check_set(const minigraph::VertexSet&);
void backend_host_configure_pool();
bool backend_host_check_container(const minigraph::ManagedContainer&);

int main() {
    minigraph::Graph graph;
    graph.num_vertex = 7;
    minigraph::Context context(2);
    context.per_thread_result.at(0) += 42;
    backend_host_configure_pool();
    minigraph::VertexSet set(8);
    set.set_size(8);
    for (size_t i = 0; i < 8; ++i) set[i] = i;
    minigraph::ManagedContainer container(8);
    for (size_t i = 0; i < 8; ++i) container[i] = i;
    container.Resize(2049);
    // Linking checks type identity; this checks layout and live host access.
    return backend_host_check(graph, context) && context.get_result() == 42 &&
           backend_host_check_set(set) && backend_host_check_container(container) &&
           minigraph::internal::VertexSetPool::TOTAL_ALLOCATED == 16 * sizeof(minigraph::IdType) ? 0 : 1;
}
