#pragma once
#ifdef GRAPHMINI_USE_BACKEND_MODULE
import graphmini.backend;
#elif defined(GRAPHMINI_USE_HEADER_UNIT)
import "../backend/backend.h";
#else
#include <cstdint>
#include "../backend/backend.h"
#endif
namespace minigraph
{
    using GraphType = Graph;
    using VertexSetType = VertexSet;
    void plan(const GraphType* graph, Context& ctx);
    uint64_t pattern_size();
}
