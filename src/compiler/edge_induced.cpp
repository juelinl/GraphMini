#include "compiler/planning.h"
namespace minigraph {
PlanIR compile_edge_induced(const std::string &query, CodeGenConfig config, MetaData meta) {
    config.adjMatType = AdjMatType::EdgeInduced;
    return build_plan(query, config, meta).plan;
}
} // namespace minigraph
