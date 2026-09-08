#pragma once
#include "codegen.h"
#include "compiler/execution_ir.h"
#include "ir.h"
#include <set>
namespace minigraph {
class CppCodegen {
  public:
    CppCodegen(CodeGenConfig config, const ExecutionIR &execution)
        : config_(config), profiling_(config.runnerType == RunnerType::Profiling),
          execution_(execution) {}
    std::string emit_omp(PlanIR plan, CodeGenConfig config);
    std::string emit_nested(PlanIR plan, CodeGenConfig config);

  private:
    const CodeGenConfig config_;
    const bool profiling_;
    const ExecutionIR &execution_;
    static std::string gen_indent(int dep) { return std::string(dep + 4, '\t'); }
    static std::string gen_indent_tbb(int dep) { return gen_indent(dep); }
    std::string emit_read_adj(const PlanIR &plan, int dep);
    std::string emit_iter(const PlanIR &plan, int dep);
    std::string emit_op(const PlanIR &plan, const VertexSetIR &op);
    std::string gen_mg_type(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_init(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_adj(const PlanIR &plan, int dep, int indent_dep = -1);
    bool skip_build_indices(const PlanIR &plan, const MiniGraphIR &mg, const VertexSetIR &iter);
    std::string emit_mg_indice(const PlanIR &plan, const MiniGraphIR &mg, int dep);
    std::string emit_mg_build(const PlanIR &plan, const MiniGraphIR &mg);
    std::string emit_mg_op(const PlanIR &plan, const VertexSetIR &op);
    std::string emit_iep(const PlanIR &plan, size_t group_id);
    std::string gen_comment_iep(const PlanIR &plan, size_t group_id);
    std::vector<MiniGraphIR> gen_used_mg(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::vector<VertexSetIR> gen_used_set(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::set<int> gen_used_adj(const PlanIR &plan, const CodeGenConfig &config, int loop);
    std::string emit_tbb_call(const PlanIR &plan, const CodeGenConfig &config, int loop, int indent_dep);
    std::string emit_tbb_loop(const PlanIR &plan, const CodeGenConfig &config, int loop);
};
} // namespace minigraph
