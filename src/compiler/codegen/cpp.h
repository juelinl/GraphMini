#pragma once
#include <cstddef>
#include <string>
#include "compiler/codegen.h"
#include "compiler/execution_ir.h"
#include "compiler/ir.h"
#include <set>
namespace minigraph {
class CppCodegen {
  public:
    CppCodegen(CodeGenConfig config, const ExecutionIR &execution)
        : profiling_(config.runnerType == RunnerType::Profiling),
          execution_(execution), bitmap_diagnostics_(config.bitmapDiagnostics),
          graph_name_(config.parType == ParallelType::OpenMP ? "graph" : "query.graph") {}
    std::string emit_omp(PlanIR plan, CodeGenConfig config);
    std::string emit_nested(PlanIR plan, CodeGenConfig config);

  private:
    const bool profiling_;
    const ExecutionIR &execution_;
    const bool bitmap_diagnostics_;
    const char *const graph_name_;
    bool uses_selected_vertex(int dep) const;
    std::string emit_read_adj(const PlanIR &plan, int dep);
    std::string emit_iter(const PlanIR &plan, int dep);
    std::string emit_op(const PlanIR &plan, const VertexSetIR &op);
    std::string emit_bitmap_iter(const PlanIR &plan, int dep);
    std::string emit_bitmap_build(int dep);
    std::string emit_bitmap_tasks(const PlanIR &plan, int dep);
    struct BitmapEmission {
        const char *state_access;
        const char *empty_action;
    };
    std::string emit_bitmap_ops(const PlanIR &plan, int depth, const BitmapEmission &target);
    std::string gen_mg_type(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_init(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_adj(const PlanIR &plan, int dep);
    bool skip_build_indices(const PlanIR &plan, const MiniGraphIR &mg, const VertexSetIR &iter);
    std::string emit_mg_indice(const PlanIR &plan, const MiniGraphIR &mg, int dep);
    std::string emit_mg_build(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_op(const PlanIR &plan, const VertexSetIR &op);
    std::string emit_iep(const PlanIR &plan, size_t group_id);
    std::string gen_comment_iep(const PlanIR &plan, size_t group_id);
    std::vector<MiniGraphIR> gen_used_mg(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::vector<VertexSetIR> gen_used_set(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::set<int> gen_used_adj(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::string emit_tbb_call(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::string emit_tbb_loop(const PlanIR &plan, const CodeGenConfig &config, int loop);
};
} // namespace minigraph
