#include "compiler/planning.h"
namespace minigraph {
PlanIR compile_vertex_induced(const std::string &query, CodeGenConfig config, MetaData meta) {
    config.adjMatType = AdjMatType::VertexInduced;
    return build_plan(query, config, meta).plan;
}
} // namespace minigraph
