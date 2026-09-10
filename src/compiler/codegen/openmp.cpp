#include "common/types.h"
#include "compiler/codegen.h"
#include "compiler/ir.h"
#include "common/logging.h"
#include "common/timer.h"

#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include "compiler/config.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <numeric>
#include <set>
#include <sstream>
#include <utility>
namespace minigraph {
std::string CppCodegen::emit_omp(PlanIR plan, CodeGenConfig config) {
    Timer t;
    std::ostringstream out;
    if (profiling_)
        out << "#include \"plan_profile.h\"\n";
    else
        out << "#include \"plan.h\"\n";
    if (config.bitmap) {
        out << "// bitmap: " << execution_.bitmap_reason << '\n';
        if (execution_.bitmap_region)
            out << "#include \"backend/bitgraph.h\"\n";
        if (execution_.bitmap_region && execution_.bitmap_region->full_region)
            out << "#include \"backend/bitmap_dispatch.h\"\n";
    }
    out << "namespace minigraph {\n";
    if (config.bitmapDiagnostics)
        out << "static std::atomic<uint64_t> bitmap_counters[7]{};\n";
    out << "uint64_t pattern_size() {return " << plan.logical.p_size << ";}\n";
    out << "void plan(const GraphType* graph, Context& ctx){\n";
    if (profiling_)
        out << "VertexSet::profiler = ctx.profiler;\n";

    const bool needs_minigraph_alias = std::any_of(execution_.minigraphs.begin(), execution_.minigraphs.end(),
        [](const auto &entry) { return !entry.second.eager; });
    if (needs_minigraph_alias) switch (config.pruningType) {
    case (PruningType::Eager):
        out << "using MiniGraphType = MiniGraphEager;\n";
        break;
    case (PruningType::Static):
        out << "using MiniGraphType = MiniGraphLazy;\n";
        break;
    case (PruningType::Online):
        out << "using MiniGraphType = MiniGraphOnline;\n";
        break;
    case (PruningType::CostModel):
        out << "using MiniGraphType = MiniGraphCostModel;\n";
        break;
    default:
        break;
    }
    if (config.pruningType != PruningType::None)
        out << "MiniGraphIF::DATA_GRAPH = graph;\n";
    out << "internal::VertexSetPool::configure_for_graph(graph->get_maxdeg());\n";
    if (config.bitmapDiagnostics)
        out << "for (auto& value : bitmap_counters) value.store(0, std::memory_order_relaxed);\n";
    out << "#pragma omp parallel num_threads(ctx.num_threads) default(none) "
           "shared(ctx, graph" << (config.bitmapDiagnostics ? ", bitmap_counters" : "")
        << ")\n{\n";
    out << "cc &counter = "
           "ctx.per_thread_result.at(omp_get_thread_num());\n";
    out << "cc &handled = "
           "ctx.per_thread_handled.at(omp_get_thread_num());\n";
    out << "double start = omp_get_wtime();\n";
    if (config.bitmapDiagnostics)
        out << "bitmap_counters[4].store(omp_get_num_threads(), std::memory_order_relaxed);\n";
    out << "ctx.iep_redundency = " << plan.counting.iep_redundancy << ";\n";
    out << "#pragma omp for schedule(dynamic, 1) nowait\n";
    out << fmt::format("for (IdType {0} = 0; {0} < graph->get_vnum(); {0}++) {{ "
                       "// loop-0 begin\n", codegen_names::vertex(0));
    int max_dep = plan.logical.p_size - 1;
    const auto &set_ops = plan.logical.set_ops;
    switch (config.pruningType) {
    case (PruningType::None):
        if (config.adjMatType != AdjMatType::EdgeInducedIEP || plan.counting.iep_num <= 1) {
            if (execution_.bitmap_region) out << emit_search_body(plan, config, 0);
            else for (int dep = 0; dep < max_dep; dep++) {
                // code for reading adj from the graph
                out << emit_read_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << emit_op(plan, op);
                }
                // code for iterating next loop
                if (dep == plan.logical.p_size - 2)
                    continue;
                out << emit_iter(plan, dep);
            }
        } else {
            assert(plan.counting.iep_num + plan.counting.iep_depth == plan.logical.p_size - 1);
            for (int dep = 0; dep < plan.counting.iep_depth; dep++) {
                // code for reading adj from the graph
                out << emit_read_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << emit_op(plan, op);
                }
                // code for iterating next loop
                if (dep == plan.logical.p_size - 2)
                    continue;
                out << emit_iter(plan, dep);
            }
            // Code for IEP
            int dep = plan.counting.iep_depth;
            out << emit_read_adj(plan, dep);
            // code for computation at this loop
            const auto &ops = set_ops.at(dep);
            for (const auto &op : ops) {
                out << emit_op(plan, op);
            }

            for (size_t group_id = 0; group_id < plan.counting.iep_groups.size(); group_id++) {
                out << emit_iep(plan, group_id);
                out << gen_comment_iep(plan, group_id);
            }
        }
        break;

    default: // enable pruning
        if (config.adjMatType != AdjMatType::EdgeInducedIEP || plan.counting.iep_num <= 1) {
            for (int dep = 0; dep < max_dep; dep++) {
                // code for reading adj from the graph
                out << emit_read_adj(plan, dep);
                if (dep > 0)
                    out << emit_mg_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << emit_mg_op(plan, op);
                }
                if (dep == plan.logical.p_size - 2)
                    continue;
                // code for building pruned graphs
                const auto &mgs = plan.auxiliary.mg_ops.at(dep);
                for (const auto &mg : mgs) {
                    out << emit_mg_init(plan, mg);
                    out << mg;
                    out << emit_mg_build(plan, mg);
                }

                for (const auto &mg : plan.auxiliary.mg_used.at(dep + 1)) {
                    out << emit_mg_indice(plan, mg, dep);
                }
                // code for iterating next loop
                out << emit_iter(plan, dep);
            }
        } else {
            assert(plan.counting.iep_num + plan.counting.iep_depth == plan.logical.p_size - 1);
            for (int dep = 0; dep < plan.counting.iep_depth; dep++) {
                // code for reading adj from the graph
                out << emit_read_adj(plan, dep);
                if (dep > 0)
                    out << emit_mg_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << emit_mg_op(plan, op);
                }
                if (dep == plan.logical.p_size - 2)
                    continue;
                // code for building pruned graphs
                const auto &mgs = plan.auxiliary.mg_ops.at(dep);
                for (const auto &mg : mgs) {
                    out << emit_mg_init(plan, mg);
                    out << mg;
                    out << emit_mg_build(plan, mg);
                }

                for (const auto &mg : plan.auxiliary.mg_used.at(dep + 1)) {
                    out << emit_mg_indice(plan, mg, dep);
                }
                // code for iterating next loop
                out << emit_iter(plan, dep);
            }
            int dep = plan.counting.iep_depth;
            if (dep > 0)
                out << emit_mg_adj(plan, dep);
            out << emit_read_adj(plan, dep);
            // code for computation at this loop
            const auto &ops = set_ops.at(dep);
            for (const auto &op : ops) {
                out << emit_mg_op(plan, op);
            }

            for (size_t group_id = 0; group_id < plan.counting.iep_groups.size(); group_id++) {
                out << emit_iep(plan, group_id);
                out << gen_comment_iep(plan, group_id);
            }
        }
        break;
    }
    if (execution_.bitmap_region) {
        out << "handled+=1;\n}\n"; // Descendant loops are already closed.
    } else if (plan.counting.iep_num <= 1) {
        for (int dep = max_dep - 1; dep >= 0; dep--) {
            if (dep == 0)
                out << "handled+=1;\n";
            out << "}\n";
        }
    } else {
        for (int dep = plan.counting.iep_depth; dep >= 0; dep--) {
            if (dep == 0)
                out << "handled+=1;\n";
            out << "}\n";
        }
    };

    out << "ctx.per_thread_time.at(omp_get_thread_num()) = omp_get_wtime() "
           "- start;\n";
    out << "}\n";
    out << "}\n";
    out << "} // namespace minigraph \n";
    if (config.bitmapDiagnostics)
        out << "extern \"C\" uint64_t graphmini_bitmap_counter(unsigned index) { "
               "return index < 7 ? minigraph::bitmap_counters[index].load(std::memory_order_relaxed) : 0; }\n";

    out << "extern \"C\" uint64_t graphmini_pattern_size(){return "
           "minigraph::pattern_size();}\n";
    out << "extern \"C\" void graphmini_plan(const minigraph::GraphType* graph, "
           "minigraph::Context* ctx){minigraph::plan(graph, *ctx);}\n";
    out << "extern \"C\" void plan(const minigraph::GraphType* graph, "
           "minigraph::Context& ctx){return minigraph::plan(graph, ctx);};";
    LOG(INFO) << "Code Generation Time: " << ToReadableDuration(t.Passed());
    return out.str();
};

// all the mingraphs used inside the loop-th loop

} // namespace minigraph
