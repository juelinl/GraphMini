#include "common/meta.h"
#include "compiler/compilation_profile.h"
#include "compiler/codegen/cpp.h"
#include "compiler/planning.h"
#include <stdexcept>
namespace minigraph {
std::string gen_code(const std::string &query, CodeGenConfig config, MetaData meta) {
    PlanIR plan;
    {
        CompilationStage stage("planning_total");
        switch (config.adjMatType) {
        case AdjMatType::VertexInduced:
            plan = compile_vertex_induced(query, config, meta);
            break;
        case AdjMatType::EdgeInduced:
            plan = compile_edge_induced(query, config, meta);
            break;
        case AdjMatType::EdgeInducedIEP:
            plan = compile_edge_induced_iep(query, config, meta);
            break;
        default:
            throw std::invalid_argument("Unsupported query type");
        }
    }
    if (config.pruningType != PruningType::None) {
        CompilationStage stage("auxiliary_planning");
        plan = create_plan_mg(plan, config);
    }
    ExecutionIR execution;
    {
        CompilationStage stage("execution_lowering");
        execution = lower_execution(plan);
    }
    CompilationStage stage("cpp_emission");
    CppCodegen writer(config, execution);
    switch (config.parType) {
    case ParallelType::OpenMP:
        return writer.emit_omp(plan, config);
    case ParallelType::TbbTop:
    case ParallelType::Nested:
    case ParallelType::NestedRt:
        return (config.bitmap ? "// bitmap: " + execution.bitmap_reason + "\n" : "") +
               writer.emit_nested(plan, config);
    default:
        throw std::invalid_argument("Unsupported parallel type");
    }
}
} // namespace minigraph
