#include "common/meta.h"
#include "compiler/compilation_profile.h"
#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include "compiler/codegen/format.h"
#include "compiler/planning.h"
#include <stdexcept>
namespace minigraph {
std::string gen_code(const std::string &query, CodeGenConfig config, MetaData meta) {
    return gen_code(query, config, meta, schedule_query(query, config, meta));
}
std::string gen_code(const std::string &query, CodeGenConfig config, MetaData meta,
                     const ScheduleResult &schedule) {
    PlanIR plan;
    {
        CompilationStage stage("planning_total");
        auto scheduled = build_plan(query, config, meta, schedule);
        switch (config.adjMatType) {
        case AdjMatType::VertexInduced:
        case AdjMatType::EdgeInduced:
            plan = std::move(scheduled.plan);
            break;
        case AdjMatType::EdgeInducedIEP:
            plan = compile_edge_induced_iep(std::move(scheduled));
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
        return format_generated_cpp(codegen_names::guide + writer.emit_omp(plan, config));
    case ParallelType::TbbTop:
    case ParallelType::Nested:
    case ParallelType::NestedRt:
        return format_generated_cpp(std::string(codegen_names::guide) + (config.bitmap ? "// bitmap: " + execution.bitmap_reason + "\n" : "") +
               writer.emit_nested(plan, config));
    default:
        throw std::invalid_argument("Unsupported parallel type");
    }
}
} // namespace minigraph
