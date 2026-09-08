#include "codegen.h"
#include "ir.h"
#include "logging.h"
#include "timer.h"

#include "compiler/codegen/cpp.h"
#include "typedef.h"
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
        for (const auto &mg : plan.mg_ops.at(physical.depth))
            if (mg.id == id)
                result.push_back(mg);
    }
    return result;
}

std::vector<VertexSetIR> CppCodegen::gen_used_set(const PlanIR &plan, const CodeGenConfig &, int loop) {
    std::vector<VertexSetIR> result;
    for (int id : execution_.loops.at(loop).captured_sets) {
        const auto &physical = execution_.sets.at(id);
        for (const auto &set : plan.set_ops.at(physical.depth))
            if (set.id == id)
                result.push_back(set);
    }
    return result;
}

std::set<int> CppCodegen::gen_used_adj(const PlanIR &, const CodeGenConfig &, int loop) {
    return execution_.loops.at(loop).captured_adjacencies;
}

// loop: the loop at which the next parallel region is evoked
std::string CppCodegen::emit_tbb_call(const PlanIR &plan, const CodeGenConfig &config, int loop,
                                      int indent_dep) {
    std::ostringstream out;
    const auto &physical = execution_.loops.at(loop);
    if (!physical.spawn_nested)
        return "";
    assert(loop > 0);
    int iter_id = plan.iter_set.at(loop - 1).id;
    const VertexSetIR &iter = plan.iter_set.at(loop - 1);
    std::vector<MiniGraphIR> used_mg = gen_used_mg(plan, config, loop);
    std::vector<VertexSetIR> used_set = gen_used_set(plan, config, loop);
    std::set<int> used_adj = gen_used_adj(plan, config, loop);
    if (!physical.runtime_threshold) {
        out << gen_indent_tbb(indent_dep) + "if (true) ";
    } else {
        const int factor = physical.threshold_factor;
        const int avg_deg = physical.average_degree;
        if (physical.cap_threshold) {
            out << gen_indent_tbb(indent_dep) +
                       fmt::format("if (s{iter_id}.size() > std::min({factor} * "
                                   "{avg_deg}, 100)) ",
                                   fmt::arg("iter_id", iter_id), fmt::arg("avg_deg", avg_deg),
                                   fmt::arg("factor", factor));
        } else {
            out << gen_indent_tbb(indent_dep) +
                       fmt::format("if (s{iter_id}.size() > {factor} * {avg_deg}) ",
                                   fmt::arg("iter_id", iter_id), fmt::arg("avg_deg", avg_deg),
                                   fmt::arg("factor", factor));
        }
    }

    int grain_size = physical.grain_size;

    out << "{\n";
    out << gen_indent_tbb(indent_dep + 1) +
               fmt::format("tbb::parallel_for(tbb::blocked_range<size_t>(0, "
                           "s{iter_id}.size(), {grain_size}), Loop{dep}",
                           fmt::arg("grain_size", grain_size), fmt::arg("iter_id", iter_id),
                           fmt::arg("dep", loop));
    // Args
    out << "(ctx";
    for (int dep : used_adj) {
        out << fmt::format(", i{}_adj", dep);
    }

    for (auto set : used_set) {
        out << fmt::format(", s{}", set.id);
    }

    out << fmt::format(", s{}", iter_id);

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format(", m{}_s{}", mg.id, iter_id);
        }

        for (auto mg : used_mg) {
            out << fmt::format(", m{}", mg.id);
        }
    }
    out << "), tbb::auto_partitioner()";
    out << "); continue;\n";
    out << gen_indent_tbb(indent_dep) << "}\n";
    return out.str();
}

std::string CppCodegen::emit_tbb_loop(const PlanIR &plan, const CodeGenConfig &config, int loop) {
    std::ostringstream out;
    int iter_id = -1;
    VertexSetIR iter;
    if (loop > 0) {
        iter_id = plan.iter_set.at(loop - 1).id;
        iter = plan.iter_set.at(loop - 1);
    }
    std::vector<MiniGraphIR> used_mg = gen_used_mg(plan, config, loop);
    std::vector<VertexSetIR> used_set = gen_used_set(plan, config, loop);
    std::set<int> used_adj = gen_used_adj(plan, config, loop);

    out << "\tclass Loop" << loop << "\n\t{\n";

    // Private Variables
    out << "\tprivate:\n";
    out << "\t\tContext& ctx;\n";

    if (!used_adj.empty())
        out << "\t\t// Adjacent Lists\n";
    for (int dep : used_adj) {
        out << fmt::format("\t\tVertexSet& i{}_adj;\n", dep);
    }

    if (!used_set.empty())
        out << "\t\t// Parent Intermediates\n";
    for (auto set : used_set) {
        out << fmt::format("\t\tVertexSet& s{};\n", set.id);
    }

    if (loop > 0)
        out << "\t\t// Iterate Set\n" << fmt::format("\t\tVertexSet& s{};\n", iter_id);

    if (config.pruningType != PruningType::None) {
        if (!plan.mg_used.at(loop).empty())
            out << "\t\t// MiniGraphs Indices\n";
        for (auto mg : plan.mg_used.at(loop)) {
            if (!skip_build_indices(plan, mg, iter))
                out << fmt::format("\t\tManagedContainer& m{}_s{};\n", mg.id, iter_id);
        }

        if (!used_mg.empty())
            out << "\t\t// MiniGraphs\n";
        for (auto mg : used_mg) {
            std::string mgType = gen_mg_type(plan, mg);
            out << fmt::format("\t\t{}& m{};\n", mgType, mg.id);
        }
    }
    out << "\tpublic:\n";
    // Constructor
    // Args
    out << "\t\tLoop" << loop << "(Context& _ctx";

    for (int dep : used_adj) {
        out << fmt::format(", VertexSet& _i{}_adj", dep);
    }

    for (auto set : used_set) {
        out << fmt::format(", VertexSet& _s{}", set.id);
    }

    if (loop > 0)
        out << ", VertexSet& _s" << iter_id;

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.mg_used.at(loop)) {
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
    out << ":ctx{_ctx}";

    for (int dep : used_adj) {
        out << fmt::format(", i{}_adj", dep) << "{" << fmt::format("_i{}_adj", dep) << "}";
    }

    for (auto set : used_set) {
        out << fmt::format(", s{}", set.id) << "{" << fmt::format("_s{}", set.id) << "}";
    }

    if (loop > 0)
        out << fmt::format(", s{}", iter_id) << "{" << fmt::format("_s{}", iter_id) << "}";

    if (config.pruningType != PruningType::None) {
        for (auto mg : plan.mg_used.at(loop)) {
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
    out << "\t\tvoid operator()(const tbb::blocked_range<size_t> &r) const {// "
           "operator begin\n";
    out << "\t\t\tconst int worker_id = "
           "tbb::this_task_arena::current_thread_index();\n";
    out << "\t\t\tcc& counter = ctx.per_thread_result.at(worker_id);\n";
    if (loop > 0) {
        out << "\t\t\t"
            << fmt::format("for (size_t i{loop}_idx = r.begin(); i{loop}_idx < "
                           "r.end(); i{loop}_idx++)",
                           fmt::arg("loop", loop));
    } else {
        //            out << "\t\t\tcc& handled =
        //            ctx.per_thread_handled.at(worker_id);\n"; out << "\t\t\t" <<
        //            "double& time = ctx.per_thread_time.at(worker_id);\n"; out <<
        //            "\t\t\t" << "tick_count t1 = tick_count::now();\n";
        out << "\t\t\t"
            << fmt::format("for (size_t i{loop}_id = r.begin(); i{loop}_id < "
                           "r.end(); i{loop}_id++)",
                           fmt::arg("loop", loop));
    }
    out << " { // loop-" << loop << "begin\n";
    int max_dep = plan.p_size - 1;
    const auto &set_ops = plan.set_ops;
    switch (config.pruningType) {
    case (PruningType::None):
        if (config.adjMatType != AdjMatType::EdgeInducedIEP || plan.iep_num <= 1) {
            for (int dep = loop; dep < max_dep; dep++) {
                int indent_dep = dep - loop;
                // code for reading adj from the graph
                out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << gen_indent_tbb(indent_dep) << emit_op(plan, op);
                    out << gen_indent_tbb(indent_dep) << op;
                }
                // skip iterating next loop
                if (dep == plan.p_size - 2)
                    continue;

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1, indent_dep);

                // code for serial executing next loop
                out << gen_indent_tbb(indent_dep) << emit_iter(plan, dep);
            }
        } else {
            assert(plan.iep_num + plan.iep_depth == plan.p_size - 1);
            for (int dep = loop; dep < plan.iep_depth; dep++) {
                int indent_dep = dep - loop;

                // code for reading adj from the graph
                out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << gen_indent_tbb(indent_dep) << emit_op(plan, op);
                    out << gen_indent_tbb(indent_dep) << op;
                }

                // code for iterating next loop
                if (dep == plan.p_size - 2)
                    continue;

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1, indent_dep);

                out << gen_indent_tbb(indent_dep) << emit_iter(plan, dep);
            }
            // Code for IEP
            int dep = plan.iep_depth;
            int indent_dep = dep - loop;

            out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
            // code for computation at this loop
            const auto &ops = set_ops.at(dep);
            for (const auto &op : ops) {
                out << gen_indent_tbb(indent_dep) << emit_op(plan, op);
                out << gen_indent_tbb(indent_dep) << op;
            }

            for (size_t group_id = 0; group_id < plan.iep_groups.size(); group_id++) {
                out << gen_indent_tbb(indent_dep) << emit_iep(plan, group_id);
                out << gen_indent_tbb(indent_dep) << gen_comment_iep(plan, group_id);
            }
        }
        break;

    default: // enable pruning
        if (config.adjMatType != AdjMatType::EdgeInducedIEP || plan.iep_num <= 1) {
            for (int dep = loop; dep < max_dep; dep++) {
                // code for reading adj from the graph
                int indent_dep = dep - loop;
                out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
                if (dep > 0)
                    out << emit_mg_adj(plan, dep, indent_dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_op(plan, op);
                    out << gen_indent_tbb(indent_dep) << op;
                }
                if (dep == plan.p_size - 2)
                    continue;
                // code for building pruned graphs
                const auto &mgs = plan.mg_ops.at(dep);
                for (const auto &mg : mgs) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_init(plan, mg);
                    out << gen_indent_tbb(indent_dep) << mg;
                    out << gen_indent_tbb(indent_dep) << emit_mg_build(plan, mg);
                }

                for (const auto &mg : plan.mg_used.at(dep + 1)) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_indice(plan, mg, dep);
                }

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1, indent_dep);

                // code for serially iterating next loop
                out << gen_indent_tbb(indent_dep) << emit_iter(plan, dep);
            }
        } else {
            assert(plan.iep_num + plan.iep_depth == plan.p_size - 1);
            for (int dep = loop; dep < plan.iep_depth; dep++) {
                int indent_dep = dep - loop;
                // code for reading adj from the graph
                out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
                if (dep > 0)
                    out << emit_mg_adj(plan, dep, indent_dep);
                // code for computation at this loop
                const auto &ops = set_ops.at(dep);
                for (const auto &op : ops) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_op(plan, op);
                    out << gen_indent_tbb(indent_dep) << op;
                }
                if (dep == plan.p_size - 2)
                    continue;
                // code for building pruned graphs
                const auto &mgs = plan.mg_ops.at(dep);
                for (const auto &mg : mgs) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_init(plan, mg);
                    out << gen_indent_tbb(indent_dep) << mg;
                    out << gen_indent_tbb(indent_dep) << emit_mg_build(plan, mg);
                }

                for (const auto &mg : plan.mg_used.at(dep + 1)) {
                    out << gen_indent_tbb(indent_dep) << emit_mg_indice(plan, mg, dep);
                }

                // code for calling parallel nested loop
                out << emit_tbb_call(plan, config, dep + 1, indent_dep);

                // code for iterating next loop
                out << gen_indent_tbb(indent_dep) << emit_iter(plan, dep);
            }
            int dep = plan.iep_depth;
            int indent_dep = dep - loop;
            if (dep > 0)
                out << emit_mg_adj(plan, dep, indent_dep);
            out << gen_indent_tbb(indent_dep) << emit_read_adj(plan, dep);
            // code for computation at this loop
            const auto &ops = set_ops.at(dep);
            for (const auto &op : ops) {
                out << gen_indent_tbb(indent_dep) << emit_mg_op(plan, op);
                out << gen_indent_tbb(indent_dep) << op;
            }

            for (size_t group_id = 0; group_id < plan.iep_groups.size(); group_id++) {
                out << gen_indent_tbb(indent_dep) << emit_iep(plan, group_id);
                out << gen_indent_tbb(indent_dep) << gen_comment_iep(plan, group_id);
            }
        }
        break;
    }
    if (plan.iep_num <= 1) {
        for (int dep = max_dep - 1; dep >= loop; dep--) {
            int indent_dep = dep - loop;
            //                if (dep == 0 && loop == 0) out << gen_indent(dep) <<
            //                "handled += 1;\n";
            out << gen_indent_tbb(indent_dep) << "} // loop-" << std::to_string(dep) << " end\n";
        }
    } else {
        for (int dep = plan.iep_depth; dep >= loop; dep--) {
            int indent_dep = dep - loop;
            //                if (dep == 0 && loop == 0) out << gen_indent(dep) <<
            //                "handled += 1;\n";
            out << gen_indent_tbb(indent_dep) << "} // loop-" << std::to_string(dep) << " end\n";
        }
    };

    // if (loop <= 2) out << "\t\t\tctx.per_thread_tick.at(worker_id) =
    // tick_count::now();\n";
    out << "\t\t} // operator end\n";
    out << "\t}; // Loop\n\n";
    return out.str();
}

std::string CppCodegen::emit_nested(PlanIR plan, CodeGenConfig config) {
    Timer t;
    std::ostringstream out;
    if (profiling_)
        out << "#include \"plan_profile.h\"\n";
    else
        out << "#include \"plan.h\"\n";
    // out << "#include \"oneapi/tbb/parallel_for.h\"\n";
    out << "namespace minigraph {\n";
    out << "\tuint64_t pattern_size() {return " << plan.p_size << ";}\n";
    out << "\tstatic const Graph * graph;\n";

    switch (config.pruningType) {
    case (PruningType::Eager):
        out << "\tusing MiniGraphType = MiniGraphEager;\n";
        break;
    case (PruningType::Static):
        out << "\tusing MiniGraphType = MiniGraphLazy;\n";
        break;
    case (PruningType::Online):
        out << "\tusing MiniGraphType = MiniGraphOnline;\n";
        break;
    case (PruningType::CostModel):
        out << "\tusing MiniGraphType = MiniGraphCostModel;\n";
        break;
    default:
        break;
    }
    for (int loop = plan.get_serial_loop() - 1; loop >= 0; loop--) {
        out << emit_tbb_loop(plan, config, loop);
    }
    out << "\tvoid plan(const GraphType* _graph, Context& ctx){ // plan \n";
    if (profiling_) {
        out << "\t\tVertexSet::profiler = ctx.profiler;\n";
    }
    out << "\t\tctx.tick_begin = tbb::tick_count::now();\n";
    out << "\t\tctx.iep_redundency = " << plan.iep_redundancy << ";\n";
    out << "\t\tgraph = _graph;\n";
    if (config.pruningType != PruningType::None)
        out << "\t\tMiniGraphIF::DATA_GRAPH = graph;\n";
    out << "\t\tinternal::VertexSetPool::configure_for_graph(graph->get_maxdeg())"
           ";\n";
    out << "\t\ttbb::parallel_for(tbb::blocked_range<size_t>(0, "
           "graph->get_vnum()), Loop0(ctx), tbb::simple_partitioner());\n";
    out << "\t} // plan\n";
    out << "} // minigraph\n";
    out << "extern \"C\" uint64_t graphmini_pattern_size(){return "
           "minigraph::pattern_size();}\n";
    out << "extern \"C\" void graphmini_plan(const minigraph::GraphType* graph, "
           "minigraph::Context* ctx){minigraph::plan(graph, *ctx);}\n";
    return out.str();
}

} // namespace minigraph
