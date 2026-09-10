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
std::vector<MiniGraphIR> CppCodegen::gen_used_mg(const PlanIR &plan, const CodeGenConfig &, int loop) {
    std::vector<MiniGraphIR> result;
    for (int id : execution_.loops.at(loop).captured_minigraphs) {
        const auto &physical = execution_.minigraphs.at(id);
        for (const auto &mg : plan.auxiliary.mg_ops.at(physical.depth))
            if (mg.id == id)
                result.push_back(mg);
    }
    return result;
}

std::vector<VertexSetIR> CppCodegen::gen_used_set(const PlanIR &plan, const CodeGenConfig &, int loop) {
    std::vector<VertexSetIR> result;
    for (int id : execution_.loops.at(loop).captured_sets) {
        const auto &physical = execution_.sets.at(id);
        for (const auto &set : plan.logical.set_ops.at(physical.depth))
            if (set.id == id)
                result.push_back(set);
    }
    return result;
}

std::set<int> CppCodegen::gen_used_adj(const PlanIR &, const CodeGenConfig &, int loop) {
    return execution_.loops.at(loop).captured_adjacencies;
}

// loop: the loop at which the next parallel region is evoked
std::string CppCodegen::emit_tbb_call(const PlanIR &plan, const CodeGenConfig &config, int loop) {
    std::ostringstream out;
    const auto &physical = execution_.loops.at(loop);
    if (!physical.spawn_nested)
        return "";
    assert(loop > 0);
    int iter_id = plan.logical.iter_set.at(loop - 1).id;
    const VertexSetIR &iter = plan.logical.iter_set.at(loop - 1);
    std::vector<MiniGraphIR> used_mg = gen_used_mg(plan, config, loop);
    std::vector<VertexSetIR> used_set = gen_used_set(plan, config, loop);
    std::set<int> used_adj = gen_used_adj(plan, config, loop);
    if (!physical.runtime_threshold) {
        out << "if (true) ";
    } else {
        const int factor = physical.threshold_factor;
        const int avg_deg = physical.average_degree;
        if (physical.cap_threshold) {
            out << fmt::format("if (s{iter_id}.size() > std::min({factor} * "
                                   "{avg_deg}, 100)) ",
                                   fmt::arg("iter_id", iter_id), fmt::arg("avg_deg", avg_deg),
                                   fmt::arg("factor", factor));
        } else {
            out << fmt::format("if (s{iter_id}.size() > {factor} * {avg_deg}) ",
                                   fmt::arg("iter_id", iter_id), fmt::arg("avg_deg", avg_deg),
                                   fmt::arg("factor", factor));
        }
    }

    int grain_size = physical.grain_size;

    out << "{\n";
    out << fmt::format("tbb::parallel_for(tbb::blocked_range<size_t>(0, "
                           "s{iter_id}.size(), {grain_size}), {level}",
                           fmt::arg("grain_size", grain_size), fmt::arg("iter_id", iter_id),
                           fmt::arg("level", codegen_names::set_level(loop)));
    // Args
    out << "(query";
    for (int dep : used_adj) {
        out << fmt::format(", {}", codegen_names::adjacency(dep));
    }

    for (auto set : used_set) {
        out << fmt::format(", s{}", set.id);
    }

    out << fmt::format(", s{}", iter_id);

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.auxiliary.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format(", m{}_s{}", mg.id, iter_id);
        }

        for (auto mg : used_mg) {
            out << fmt::format(", m{}", mg.id);
        }
    }
    out << "), tbb::auto_partitioner()";
    out << "); continue;\n";
    out << "}\n";
    return out.str();
}

std::string CppCodegen::emit_tbb_loop(const PlanIR &plan, const CodeGenConfig &config, int loop) {
    std::ostringstream out;
    int iter_id = -1;
    VertexSetIR iter;
    if (loop > 0) {
        iter_id = plan.logical.iter_set.at(loop - 1).id;
        iter = plan.logical.iter_set.at(loop - 1);
    }
    std::vector<MiniGraphIR> used_mg = gen_used_mg(plan, config, loop);
    std::vector<VertexSetIR> used_set = gen_used_set(plan, config, loop);
    std::set<int> used_adj = gen_used_adj(plan, config, loop);

    out << "class " << codegen_names::set_level(loop) << "\n{\n";

    // Private Variables
    out << "private:\n";
    out << "const QueryContext& query;\n";

    if (!used_adj.empty())
        out << "// Adjacent Lists\n";
    for (int dep : used_adj) {
        out << fmt::format("VertexSet& {};\n", codegen_names::adjacency(dep));
    }

    if (!used_set.empty())
        out << "// Parent Intermediates\n";
    for (auto set : used_set) {
        out << fmt::format("VertexSet& s{};\n", set.id);
    }

    if (loop > 0)
        out << "// Iterate Set\n" << fmt::format("VertexSet& s{};\n", iter_id);

    if (config.pruningType != PruningType::None) {
        if (!plan.auxiliary.mg_used.at(loop).empty())
            out << "// MiniGraphs Indices\n";
        for (auto mg : plan.auxiliary.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format("ManagedContainer& m{}_s{};\n", mg.id, iter_id);
        }

        if (!used_mg.empty())
            out << "// MiniGraphs\n";
        for (auto mg : used_mg) {
            std::string mgType = gen_mg_type(plan, mg);
            out << fmt::format("{}& m{};\n", mgType, mg.id);
        }
    }
    out << "public:\n";
    // Constructor
    // Args
    out << codegen_names::set_level(loop) << "(const QueryContext& _query";

    for (int dep : used_adj) {
        out << fmt::format(", VertexSet& _{}", codegen_names::adjacency(dep));
    }

    for (auto set : used_set) {
        out << fmt::format(", VertexSet& _s{}", set.id);
    }

    if (loop > 0)
        out << ", VertexSet& _s" << iter_id;

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.auxiliary.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format(", ManagedContainer& _m{}_s{}", mg.id, iter_id);
        }

        for (auto mg : used_mg) {
            std::string mgType = gen_mg_type(plan, mg);
            out << fmt::format(", {}& _m{}", mgType, mg.id);
        }
    }
    out << ")";

    // Initialization
    out << ":query{_query}";

    for (int dep : used_adj) {
        out << fmt::format(", {}", codegen_names::adjacency(dep)) << "{" << fmt::format("_{}", codegen_names::adjacency(dep)) << "}";
    }

    for (auto set : used_set) {
        out << fmt::format(", s{}", set.id) << "{" << fmt::format("_s{}", set.id) << "}";
    }

    if (loop > 0)
        out << fmt::format(", s{}", iter_id) << "{" << fmt::format("_s{}", iter_id) << "}";

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.auxiliary.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format(", m{}_s{}", mg.id, iter_id) << "{"
                    << fmt::format(" _m{}_s{}", mg.id, iter_id) << "}";
        }

        for (auto mg : used_mg) {
            out << fmt::format(", m{}", mg.id) << "{" << fmt::format("_m{}", mg.id) << "}";
        }
    }
    out << " {};\n";

    // Operator
    out << "void operator()(const tbb::blocked_range<size_t> &r) const {\n";
    out << "auto& ctx = query.ctx;\n";
    out << "const int worker_id = "
           "tbb::this_task_arena::current_thread_index();\n";
    out << "cc& counter = ctx.per_thread_result.at(worker_id);\n";
    if (loop > 0) {
        out << fmt::format("for (size_t {idx} = r.begin(); {idx} < "
                           "r.end(); {idx}++)",
                           fmt::arg("idx", codegen_names::index(loop)));
    } else {
        out << fmt::format("for (size_t {vertex} = r.begin(); {vertex} < "
                           "r.end(); {vertex}++)",
                           fmt::arg("vertex", codegen_names::vertex(loop)));
    }
    out << " { // loop-" << loop << "begin\n";
    if (loop == 0 && !profiling_)
        out << fmt::format("BenchmarkRootProgress root_progress(query.progress.root({}));\n", codegen_names::vertex(0));
    int max_dep = plan.logical.p_size - 1;
    const auto &set_ops = plan.logical.set_ops;
    switch (config.pruningType) {
    case (PruningType::None):
        if (config.adjMatType != AdjMatType::EdgeInducedIEP || plan.counting.iep_num <= 1) {
            for (int dep = loop; dep < max_dep; dep++) {
                // code for reading adj from the graph
                out << emit_read_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << emit_op(plan, op);
                }
                // skip iterating next loop
                if (dep == plan.logical.p_size - 2)
                    continue;

                // code for calling parallel nested loop
                out << emit_bitmap_build(dep);
                if (!execution_.bitmap_region || dep > execution_.bitmap_region->entry_depth)
                    out << emit_tbb_call(plan, config, dep + 1);

                // code for serial executing next loop
                out << emit_iter(plan, dep);
            }
        } else {
            assert(plan.counting.iep_num + plan.counting.iep_depth == plan.logical.p_size - 1);
            for (int dep = loop; dep < plan.counting.iep_depth; dep++) {

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

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1);

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
            for (int dep = loop; dep < max_dep; dep++) {
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

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1);

                // code for serially iterating next loop
                out << emit_iter(plan, dep);
            }
        } else {
            assert(plan.counting.iep_num + plan.counting.iep_depth == plan.logical.p_size - 1);
            for (int dep = loop; dep < plan.counting.iep_depth; dep++) {
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

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1);

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
    if (plan.counting.iep_num <= 1) {
        for (int dep = max_dep - 1; dep >= loop; dep--) {
            out << "}\n";
            if (execution_.bitmap_region && dep == execution_.bitmap_region->entry_depth + 1)
                out << "} // array fallback\n";
        }
    } else {
        for (int dep = plan.counting.iep_depth; dep >= loop; dep--) {
            out << "}\n";
        }
    };

    // if (loop <= 2) out << "ctx.per_thread_tick.at(worker_id) =
    // tick_count::now();\n";
    out << "}\n";
    out << "};\n\n";
    return out.str();
}

std::string CppCodegen::emit_nested(PlanIR plan, CodeGenConfig config) {
    Timer t;
    std::ostringstream out;
    if (profiling_)
        out << "#include \"plan_profile.h\"\n";
    else
        out << "#include \"plan.h\"\n";
    if (!profiling_) out << "#include \"backend/benchmark_progress.h\"\n";
    if (execution_.bitmap_region) out << "#include \"backend/bitmap_tasks.h\"\n";
    if (execution_.bitmap_region && execution_.bitmap_region->full_region)
        out << "#include \"backend/bitmap_dispatch.h\"\n";
    // out << "#include \"oneapi/tbb/parallel_for.h\"\n";
    out << "namespace minigraph {\n";
    out << "// Borrowed per-query state; all task joins complete before plan returns.\n"
           "struct QueryContext {\n"
           "const Graph* const graph;\n"
           "Context& ctx;\n";
    if (!profiling_) out << "BenchmarkProgress& progress;\n";
    out << "};\n";
    if (execution_.bitmap_region)
        out << "static const auto bitmap_task_policy = BitmapTaskPolicy::from_environment();\n";
    if (config.bitmapDiagnostics) out << "static std::atomic<uint64_t> bitmap_counters[7]{};\n";
    out << "uint64_t pattern_size() {return " << plan.logical.p_size << ";}\n";
    out << emit_bitmap_levels(plan);

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
    for (int loop = execution_.serial_loop_boundary - 1; loop >= 0; loop--) {
        if (loop > 0 && execution_.bitmap_region) {
            auto arrays = execution_;
            arrays.bitmap_region.reset();
            out << CppCodegen(config, arrays).emit_tbb_loop(plan, config, loop);
        } else out << emit_tbb_loop(plan, config, loop);
    }
    out << "void plan(const GraphType* graph, Context& ctx){\n";
    if (profiling_) {
        out << "VertexSet::profiler = ctx.profiler;\n";
    }
    out << "ctx.tick_begin = tbb::tick_count::now();\n";
    if (config.bitmapDiagnostics)
        out << "for (auto& value : bitmap_counters) value.store(0, std::memory_order_relaxed);\n"
               "bitmap_counters[4].store(ctx.num_threads, std::memory_order_relaxed);\n";
    out << "ctx.iep_redundency = " << plan.counting.iep_redundancy << ";\n";
    if (!profiling_)
        out << "BenchmarkProgress progress(ctx, graph->get_vnum());\n";
    out << "const QueryContext query{graph, ctx" << (profiling_ ? "" : ", progress") << "};\n";
    if (config.pruningType != PruningType::None)
        out << "MiniGraphIF::DATA_GRAPH = graph;\n";
    out << "internal::VertexSetPool::configure_for_graph(graph->get_maxdeg())"
           ";\n";
    out << "tbb::parallel_for(tbb::blocked_range<size_t>(0, "
        << "graph->get_vnum()), " << codegen_names::set_level(0) << "(query), tbb::simple_partitioner());\n";
    out << "}\n";
    out << "} // minigraph\n";
    if (config.bitmapDiagnostics)
        out << "extern \"C\" uint64_t graphmini_bitmap_counter(unsigned index) { return index < 7 ? minigraph::bitmap_counters[index].load(std::memory_order_relaxed) : 0; }\n";
    out << "extern \"C\" uint64_t graphmini_pattern_size(){return "
           "minigraph::pattern_size();}\n";
    out << "extern \"C\" void graphmini_plan(const minigraph::GraphType* graph, "
           "minigraph::Context* ctx){minigraph::plan(graph, *ctx);}\n";
    return out.str();
}

} // namespace minigraph
