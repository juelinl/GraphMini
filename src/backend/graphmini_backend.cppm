module;
// Keep header-defined types in the global module: the C++17 Python host passes
// Graph/Context across the query DSO boundary. Attaching them to a named module
// would create different C++ entities. Export aliases, as oneTBB does itself.
#include "backend.h"

export module graphmini.backend;
export import tbb;

export using ::size_t;
export using ::uint64_t;
export using ::omp_get_thread_num;
export using ::omp_get_wtime;
export namespace minigraph {
    namespace internal {
        using ::minigraph::internal::VertexSetPool;
    }
    using ::minigraph::IdType;
    using ::minigraph::VertexSet;
    using ::minigraph::IEPBitmap;
    using ::minigraph::Graph;
    using ::minigraph::Context;
    using ::minigraph::cc;
    using ::minigraph::ManagedContainer;
    using ::minigraph::MiniGraphIF;
    using ::minigraph::MiniGraphEager;
    using ::minigraph::MiniGraphLazy;
    using ::minigraph::MiniGraphOnline;
    using ::minigraph::MiniGraphCostModel;
}
