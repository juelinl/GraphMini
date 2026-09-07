import graphmini.backend;

bool backend_host_check(const minigraph::Graph&, const minigraph::Context&);

int main() {
    minigraph::Graph graph;
    graph.num_vertex = 7;
    minigraph::Context context(2);
    context.per_thread_result.at(0) += 42;
    // Linking checks type identity; this checks layout and live host access.
    return backend_host_check(graph, context) && context.get_result() == 42 ? 0 : 1;
}
