#ifdef GRAPHMINI_PROFILE_RUNTIME
#include "backend_prof/vertex_set.h"
#else
#include "backend/vertex_set.h"
#endif
#include <stdexcept>

using namespace minigraph;

int main() {
    internal::VertexSetPool::configure_for_graph(8);
    IdType data[]{1, 3, 5};
    VertexSet source(7, data, 3);
    auto removed = source.remove(3).remove(99);
    auto bounded = source.remove(3).bounded(5);
    if (!removed.pooled() || !bounded.pooled())
        throw std::runtime_error("Temporary view lost workspace ownership");
    VertexSet churn(8);
    churn.set_size(8);
    for (size_t i = 0; i < churn.size(); ++i)
        churn[i] = 99;
    if (removed.size() != 2 || removed[0] != 1 || removed[1] != 5 ||
        bounded.size() != 1 || bounded[0] != 1)
        throw std::runtime_error("Recycled workspace corrupted a live set");
    if (source.bounded(5).pooled() || source.remove(99).pooled())
        throw std::runtime_error("Lvalue views unexpectedly acquired ownership");
}
