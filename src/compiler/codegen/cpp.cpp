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
std::string CppCodegen::emit_read_adj(const PlanIR &plan, int dep) {
    std::string out;
    if (profiling_) {
        out += fmt::format("ctx.profiler->set_cur_loop({dep});\n", fmt::arg("dep", dep));
        out += gen_indent(dep);
    }
    bool NoAdjNeeded = true;
    if (config_.pruningType == PruningType::None) {
        NoAdjNeeded = false;
    } else {
        for (const auto &op : plan.set_ops.at(dep)) {
            auto mg = plan.get_parent_mg(op);
            NoAdjNeeded = NoAdjNeeded && mg.has_value();
        }
    }
    if (dep > 0) {
        const VertexSetIR &iter = plan.iter_set.at(dep - 1);
        out += fmt::format("const IdType i{dep}_id = s{iter_id}[i{dep}_idx];\n", fmt::arg("dep", dep),
                           fmt::arg("iter_id", iter.id));
    }

    if (!NoAdjNeeded) {
        if (dep > 0)
            out += gen_indent(dep);
        out += fmt::format("VertexSet i{dep}_adj = graph->N(i{dep}_id);\n", fmt::arg("dep", dep));
    }
    return out;
}

std::string CppCodegen::emit_iter(const PlanIR &plan, int dep) {
    if (dep >= plan.p_size - 2) {
        return "";
    } else {
        const auto &iter_set = plan.iter_set.at(dep);
        return fmt::format("for (size_t i{dep}_idx = 0; i{dep}_idx < s{iter_id}.size(); "
                           "i{dep}_idx++) {left} // loop-{dep} begin\n",
                           fmt::arg("left", "{"), fmt::arg("iter_id", iter_set.id), fmt::arg("dep", dep + 1));
    }
}

std::string CppCodegen::emit_op(const PlanIR &plan, const VertexSetIR &op) {
    std::string out;
    auto parent = plan.get_parent_vset(op);
    int dep = op.loop_depth();
    std::string upper_bound;
    if (op.is_restricted(op.loop_depth())) {
        upper_bound = fmt::format(", i{}_adj.vid()", dep);
    }
    if (parent.has_value()) {
        CHECK(parent->loop_depth() + 1 >= op.loop_depth()) << "Not optimal parent";
        // generate code from prefix
        if (parent->loop_depth() == op.loop_depth()) {
            CHECK(op.is_restricted(op.loop_depth()))
                << "\nLogic error (op is not restricted at loop_depth)\nOP:\n"
                << op << "\nParent:\n"
                << parent.value();
            out += fmt::format("VertexSet s{op_id} = s{parent_id}.bounded(i{dep}_id);\n",
                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                               fmt::arg("dep", dep));

        } else if (parent->loop_depth() == op.loop_depth() - 1) {

            if (op.is_edge(op.loop_depth())) {
                // Both VertexInduced and EdgeInduced: intersection
                if (!plan.is_last_op(op)) {
                    // intersect and return vertex set
                    out += fmt::format("VertexSet s{op_id} = "
                                       "s{parent_id}.intersect(i{dep}_adj{upper_bound});\n",
                                       fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                       fmt::arg("dep", dep), fmt::arg("upper_bound", upper_bound));
                } else {
                    // intersect and return counter
                    out += fmt::format("counter += "
                                       "s{parent_id}.intersect_cnt(i{dep}_adj{upper_bound});\n",
                                       fmt::arg("parent_id", parent->id), fmt::arg("dep", dep),
                                       fmt::arg("upper_bound", upper_bound));
                }
            } else {
                if (plan.config.adjMatType == minigraph::AdjMatType::VertexInduced) {
                    // VertexInduced: subtraction
                    if (!plan.is_last_op(op)) {
                        // last op: intersect and return vertex set
                        out += fmt::format("VertexSet s{op_id} = "
                                           "s{parent_id}.subtract(i{dep}_adj{upper_bound});\n",
                                           fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                           fmt::arg("dep", dep), fmt::arg("upper_bound", upper_bound));
                    } else {
                        // intersect and return counter
                        out += fmt::format("counter += "
                                           "s{parent_id}.subtract_cnt(i{dep}_adj{upper_bound});\n",
                                           fmt::arg("parent_id", parent->id), fmt::arg("dep", dep),
                                           fmt::arg("upper_bound", upper_bound));
                    }
                } else {
                    // EdgeInduced: remove v_iter
                    if (!plan.is_last_op(op)) {
                        if (op.is_restricted(op.loop_depth())) {
                            out += fmt::format("VertexSet s{op_id} = "
                                               "s{parent_id}.bounded(i{dep}_adj.vid());\n",
                                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                               fmt::arg("dep", dep));
                        } else {
                            out += fmt::format("VertexSet s{op_id} = "
                                               "s{parent_id}.remove(i{dep}_adj.vid());\n",
                                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                               fmt::arg("dep", dep));
                        }
                    } else {
                        if (op.is_restricted(op.loop_depth())) {
                            out += fmt::format("counter += s{parent_id}.bounded_cnt(i{dep}_adj.vid());\n",
                                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                               fmt::arg("dep", dep));
                        } else {
                            out += fmt::format("counter += s{parent_id}.remove_cnt(i{dep}_adj.vid());\n",
                                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                                               fmt::arg("dep", dep));
                        }
                    }
                }
            }
        }
    } else {
        // no parent
        CHECK(op.edge_num() == 1 && op.is_edge(op.loop_depth()))
            << "\nLogic error: VertexSetIR should have one parent but get none\n"
            << op;

        if (op.is_restricted(op.loop_depth())) {
            out += fmt::format("VertexSet s{op_id} = i{dep}_adj.bounded(i{dep}_id)", fmt::arg("op_id", op.id),
                               fmt::arg("dep", dep));
        } else {
            out += fmt::format("VertexSet s{op_id} = i{dep}_adj", fmt::arg("op_id", op.id),
                               fmt::arg("dep", dep));
        }

        for (int subtract_id = 0; subtract_id < dep; subtract_id++) {
            if (plan.config.adjMatType == minigraph::AdjMatType::VertexInduced) {
                std::string subtract_bound;
                if (op.is_restricted(subtract_id)) {
                    subtract_bound = fmt::format(", i{}_adj.vid()", subtract_id);
                }
                //                    out += fmt::format("s{op_id} =
                //                    s{op_id}.subtract(i{subtract_id}_adj{upper_bound});\n",
                //                                   fmt::arg("op_id", op.id),
                //                                   fmt::arg("iter_id", dep),
                //                                   fmt::arg("subtract_id",
                //                                   subtract_id),
                //                                   fmt::arg("upper_bound",
                //                                   subtract_bound));
                out += fmt::format(".subtract(i{subtract_id}_adj{upper_bound})", fmt::arg("op_id", op.id),
                                   fmt::arg("iter_id", dep), fmt::arg("subtract_id", subtract_id),
                                   fmt::arg("upper_bound", subtract_bound));

            } else { // EdgeInduced
                if (op.is_restricted(subtract_id)) {
                    //                        out += fmt::format("s{op_id} =
                    //                        s{op_id}.bounded(i{subtract_id}_adj.vid());\n",
                    //                                           fmt::arg("op_id", op.id),
                    //                                           fmt::arg("iter_id", dep),
                    //                                           fmt::arg("subtract_id",
                    //                                           subtract_id));
                    out += fmt::format(".bounded(i{subtract_id}_adj.vid())", fmt::arg("op_id", op.id),
                                       fmt::arg("iter_id", dep), fmt::arg("subtract_id", subtract_id));
                } else {
                    //                        out += fmt::format("s{op_id} =
                    //                        s{op_id}.remove(i{subtract_id}_adj.vid());\n",
                    //                                           fmt::arg("op_id", op.id),
                    //                                           fmt::arg("iter_id", dep),
                    //                                           fmt::arg("subtract_id",
                    //                                           subtract_id));
                    out += fmt::format(".remove(i{subtract_id}_adj.vid())", fmt::arg("op_id", op.id),
                                       fmt::arg("iter_id", dep), fmt::arg("subtract_id", subtract_id));
                }
            }
        }
        out += ";\n";
        out +=
            gen_indent(dep) + fmt::format("if (s{op_id}.size() == 0) continue;\n", fmt::arg("op_id", op.id));
        if (plan.is_last_op(op)) {
            out += fmt::format("counter += s{op_id}.size();\n", fmt::arg("op_id", op.id));
        }
    }

    return out;
}

bool CppCodegen::mg_should_eager(const PlanIR &plan, const MiniGraphIR &mg) {
    // for (int loop_dep = mg.loop_depth() + 1; loop_dep < plan.mg_ops.size();
    // loop_dep++){
    //     for (auto& cmg: plan.mg_ops.at(loop_dep)) {
    //         if (mg.is_superset_of(cmg)) return true;
    //     }
    // }

    // if (plan.get_parent_mg(mg).has_value()) return true;

    VertexSetIR next_vertices = plan.iter_set.at(mg.loop_depth()); // next iteration
    if (!(next_vertices == mg.m_vertices))
        return false;
    for (const auto &next_intersect : plan.set_ops.at(mg.loop_depth() + 1)) {
        if (mg.computed(next_vertices, next_intersect)) {
            return true;
        }
    }
    return false;
}

std::string CppCodegen::gen_mg_type(const PlanIR &plan, const MiniGraphIR &mg) {
    return mg_should_eager(plan, mg) ? "MiniGraphEager" : "MiniGraphType";
}

std::string CppCodegen::emit_mg_init(const PlanIR &plan, const MiniGraphIR &mg) {
    //        const VertexSetIR &iter = plan.iter_set.at(mg.loop_depth());
    std::string mgType = gen_mg_type(plan, mg);
    return fmt::format("{mg_type} m{mg_id}({_bounded},{_par});\n", fmt::arg("mg_id", mg.id),
                       fmt::arg("mg_type", mgType), fmt::arg("_bounded", plan.is_bounded(mg)),
                       fmt::arg("_par", plan.is_par(mg)));
}

std::string CppCodegen::emit_mg_adj(const PlanIR &plan, int dep, int indent_dep) {
    if (dep == 0)
        return "";
    std::string indent = (indent_dep == -1) ? gen_indent(dep) : gen_indent_tbb(dep);
    const VertexSetIR &iter = plan.iter_set.at(dep - 1);
    std::string out;
    for (const auto &mg : plan.mg_used.at(dep)) {
        auto &vertices = mg.m_vertices;
        bool same_address = true;
        if (iter.loop_depth() == vertices.loop_depth()) {
            for (int i = 0; i <= iter.loop_depth(); i++) {
                if (iter.is_edge(i) != vertices.is_edge(i))
                    same_address = false;
            }
        } else {
            same_address = false;
        }

        if (same_address) {
            out += indent;
            out += fmt::format("VertexSet m{mg_id}_adj = m{mg_id}.N(i{dep}_idx);\n", fmt::arg("mg_id", mg.id),
                               fmt::arg("dep", dep));
        } else {
            std::string v_idx = fmt::format("m{mg_id}_s{iter_id}[i{dep}_idx]", fmt::arg("mg_id", mg.id),
                                            fmt::arg("iter_id", iter.id), fmt::arg("dep", dep));
            out += indent;
            out += fmt::format("VertexSet m{mg_id}_adj = m{mg_id}.N({v_idx});\n", fmt::arg("mg_id", mg.id),
                               fmt::arg("v_idx", v_idx));
        }
    }
    return out;
}

bool CppCodegen::skip_build_indices(const PlanIR &plan, const MiniGraphIR &mg, const VertexSetIR &iter) {
    auto &vertices = mg.m_vertices;
    if (iter.loop_depth() == vertices.loop_depth()) {
        bool same_address = true;
        for (int i = 0; i <= iter.loop_depth(); i++) {
            if (iter.is_edge(i) != vertices.is_edge(i))
                same_address = false;
        }
        if (same_address)
            return true;
    }
    return false;
};

std::string CppCodegen::emit_mg_indice(const PlanIR &plan, const MiniGraphIR &mg, int dep) {
    const VertexSetIR &iter = plan.iter_set.at(dep);
    if (skip_build_indices(plan, mg, iter))
        return fmt::format("//skip building indices for m{mg_id} because they can "
                           "be obtained directly\n",
                           fmt::arg("mg_id", mg.id)); // skip
    else
        return fmt::format("auto m{mg_id}_s{iter_id} = m{mg_id}.indices(s{iter_id});\n",
                           fmt::arg("mg_id", mg.id), fmt::arg("iter_id", iter.id));
};

std::string CppCodegen::emit_mg_est_visits(const PlanIR &plan, const MiniGraphIR &mg, int iter_dep) {
    int iter_id = plan.iter_set.at(mg.loop_depth()).id;
    std::string out = fmt::format("s{}.size()", iter_id);
    for (int dep = mg.loop_depth() + 1; dep < iter_dep; dep++) {
        const VertexSetIR &cur_iter = plan.iter_set.at(dep);
        auto parent = plan.get_parent_vset(cur_iter, mg.loop_depth());
        if (parent.has_value()) {
            double p1 = 1.0 * plan.meta.num_edge / plan.meta.num_vertex / plan.meta.num_vertex;
            double p2 = 1.0 * plan.meta.num_triangle * 6 * plan.meta.num_vertex / plan.meta.num_edge /
                        plan.meta.num_edge;
            double rate = 1.0;
            for (int adj_dep = mg.loop_depth(); adj_dep < cur_iter.loop_depth(); adj_dep++) {
                auto &adj_iter = plan.iter_set.at(adj_dep);
                if (adj_iter.share_at_least_one_parent_node(parent.value())) {
                    rate *= p2;
                } else {
                    rate *= p1;
                }
            }

            out += fmt::format(" * s{par_id}.size() * {rate}", fmt::arg("par_id", parent->id),
                               fmt::arg("rate", rate));
        } else {
            double avg_deg = 1.0 * plan.meta.num_edge / plan.meta.num_vertex;
            out += fmt::format(" * {}", avg_deg);
        }
    }
    return out;
}

std::string CppCodegen::emit_mg_build(const PlanIR &plan, const MiniGraphIR &mg) {
    const VertexSetIR &iter = plan.iter_set.at(mg.loop_depth());
    int iter_id = iter.id;
    std::optional<MiniGraphIR> parent_mg = plan.get_parent_mg(mg);
    std::string out;
    bool should_build_mg_eagerly = mg_should_eager(plan, mg);

    if (config_.pruningType == PruningType::CostModel && !should_build_mg_eagerly) {
        std::string factor = fmt::format("double m{mg_id}_factor = 0;\n", fmt::arg("mg_id", mg.id));
        int max_dep = std::min(plan.p_size - 2, plan.p_size - plan.iep_num - 1);

        int total_reuse = 0;
        for (int dep = mg.loop_depth() + 2; dep <= max_dep; dep++) {
            //                    const VertexSetIR &cur_iter =
            //                    plan.iter_set.at(dep-1);
            int multiplier = 0;
            for (const auto &op : plan.set_ops.at(dep)) {
                auto parent_mg = plan.get_parent_mg(op);
                if (parent_mg.has_value()) {
                    if (mg.is_superset_of(parent_mg.value()) || mg == parent_mg.value())
                        multiplier++;
                }
            }
            if (multiplier > 0) {
                std::string est_visits = emit_mg_est_visits(plan, mg, dep);
                factor +=
                    gen_indent(mg.loop_depth()) +
                    fmt::format("m{mg_id}_factor += {est_visits} * {multiplier};\n", fmt::arg("mg_id", mg.id),
                                fmt::arg("est_visits", est_visits), fmt::arg("multiplier", multiplier));
            }
            total_reuse += multiplier;
        };
        // assert(total_reuse > 0);
        out += factor;
        out += gen_indent(mg.loop_depth());
        out += fmt::format("m{mg_id}.set_reuse_multiplier(m{mg_id}_factor); ", fmt::arg("mg_id", mg.id));
    }

    if (parent_mg.has_value()) {
        out += fmt::format("m{mg_id}.build(&m{parent_id}, s{vset_id}, s{vint_id}, s{iter_id});\n",
                           fmt::arg("parent_id", parent_mg->id), fmt::arg("mg_id", mg.id),
                           fmt::arg("vset_id", mg.vset_id()), fmt::arg("vint_id", mg.vint_id()),
                           fmt::arg("iter_id", iter_id));
    } else {
        out += fmt::format("m{mg_id}.build(s{vset_id}, s{vint_id}, s{iter_id});\n", fmt::arg("mg_id", mg.id),
                           fmt::arg("vset_id", mg.vset_id()), fmt::arg("vint_id", mg.vint_id()),
                           fmt::arg("iter_id", iter_id));
    }
    return out;
};

std::string CppCodegen::emit_mg_op(const PlanIR &plan, const VertexSetIR &op) {
    std::optional<MiniGraphIR> mg = plan.get_parent_mg(op);
    if (!mg.has_value())
        return emit_op(plan, op);
    int dep = op.loop_depth();
    std::optional<VertexSetIR> parent = plan.get_parent_vset(op);
    CHECK(parent.has_value()) << "Logic error (find vset's pruned graph but not its parent)";
    VertexSetIR iter = plan.iter_set.at(op.loop_depth() - 1);
    std::string out;
    if (mg->computed(iter, op)) {
        // Read directly from the pruned graph
        if (op.is_restricted(op.loop_depth())) {
            out += fmt::format("VertexSet s{op_id} = m{mg_id}_adj.bounded(i{dep}_id);\n",
                               fmt::arg("mg_id", mg->id), fmt::arg("op_id", op.id),
                               fmt::arg("iter_id", iter.id), fmt::arg("dep", dep));
        } else {
            out += fmt::format("VertexSet s{op_id} = m{mg_id}_adj;\n", fmt::arg("mg_id", mg->id),
                               fmt::arg("op_id", op.id), fmt::arg("iter_id", iter.id), fmt::arg("dep", dep));
        }

        CHECK(!plan.is_last_op(op)) << "\nLogic error (vset should not be the last op if it can be read "
                                       "directly from a pruned graph)";
    } else if (op.is_edge(op.loop_depth())) {
        std::string upper_bound;
        if (op.is_restricted(op.loop_depth())) {
            upper_bound = fmt::format(", m{mg_id}_adj.vid()", fmt::arg("mg_id", mg->id));
        }
        if (!plan.is_last_op(op)) {
            // intersect and return vertex set
            out += fmt::format("VertexSet s{op_id} = "
                               "s{parent_id}.intersect(m{mg_id}_adj{upper_bound});\n",
                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                               fmt::arg("mg_id", mg->id), fmt::arg("upper_bound", upper_bound));
        } else {
            // intersect and return counter
            out += fmt::format("counter += s{parent_id}.intersect_cnt(m{mg_id}_adj{upper_bound});\n",
                               fmt::arg("parent_id", parent->id), fmt::arg("mg_id", mg->id),
                               fmt::arg("upper_bound", upper_bound));
        }
    } else {
        // VertexInduced: subtraction | EdgeInduced: remove one (should not arrive
        // here)
        CHECK(plan.config.adjMatType == AdjMatType::VertexInduced)
            << "Logic error (EdgeInduced patterns does not require set "
               "subtraction)";
        std::string upper_bound;
        if (op.is_restricted(op.loop_depth())) {
            upper_bound = fmt::format(", m{mg_id}_adj.vid()", fmt::arg("mg_id", mg->id));
        }
        if (!plan.is_last_op(op)) {
            // subtract and return vertex set
            out += fmt::format("VertexSet s{op_id} = "
                               "s{parent_id}.subtract(m{mg_id}_adj{upper_bound});\n",
                               fmt::arg("op_id", op.id), fmt::arg("parent_id", parent->id),
                               fmt::arg("mg_id", mg->id), fmt::arg("upper_bound", upper_bound));
        } else {
            // subtract and return counter
            out += fmt::format("counter += s{parent_id}.subtract_cnt(m{mg_id}_adj{upper_bound});\n",
                               fmt::arg("parent_id", parent->id), fmt::arg("mg_id", mg->id),
                               fmt::arg("upper_bound", upper_bound));
        }
    }
    if (!plan.is_last_op(op))
        out +=
            gen_indent(dep) + fmt::format("if (s{op_id}.size() == 0) continue;\n", fmt::arg("op_id", op.id));
    return out;
};

std::string CppCodegen::emit_iep(const PlanIR &plan, size_t group_id) {
    int val = plan.iep_vals.at(group_id);
    const auto &group = plan.iep_groups.at(group_id);
    std::string out = fmt::format("counter += {}ll", val);
    for (size_t set_id = 0; set_id < group.size(); set_id++) {
        const auto &set = group.at(set_id);
        if (set.size() == 1) {
            int set_id = set.at(0);
            const VertexSetIR &left = plan.iep_set.at(set_id);
            out += fmt::format(" * s{left_id}.size()", fmt::arg("left_id", left.id));
        } else if (set.size() == 2) {
            const VertexSetIR &left = plan.iep_set.at(set.at(0));
            const VertexSetIR &right = plan.iep_set.at(set.at(1));
            if (left == right) {
                out += fmt::format(" * s{left_id}.size()", fmt::arg("left_id", left.id));
            } else {
                out += fmt::format(" * s{left_id}.intersect_cnt(s{right_id})", fmt::arg("left_id", left.id),
                                   fmt::arg("right_id", right.id));
            }

        } else {
            const VertexSetIR &left = plan.iep_set.at(set.at(0));
            out += fmt::format(" * s{left_id}", fmt::arg("left_id", left.id));
            for (size_t i = 1; i < set.size() - 1; ++i) {
                const VertexSetIR &right = plan.iep_set.at(set.at(i));
                out += fmt::format(".intersect(s{right_id})", fmt::arg("right_id", right.id));
            }
            const VertexSetIR &right = plan.iep_set.at(set.at(set.size() - 1));
            out += fmt::format(".intersect_cnt(s{right_id})", fmt::arg("right_id", right.id));
        }
    }
    out += ";\n";
    return out;
}

std::string CppCodegen::gen_comment_iep(const PlanIR &plan, size_t group_id) {
    int val = plan.iep_vals.at(group_id);
    std::string group_str, comp_str;
    const auto &group = plan.iep_groups.at(group_id);
    size_t j = 0;
    for (const auto &set : group) {
        size_t i = 0;
        group_str += "(";
        comp_str += "|";
        for (auto set_id : set) {
            group_str += std::to_string(set_id);
            comp_str += fmt::format("VSet({})", plan.iep_set.at(set_id).id);
            if (i++ != set.size() - 1) {
                group_str += " ";
                comp_str += " & ";
            }
        }
        group_str += ")";
        comp_str += "|";
        if (j++ != group.size() - 1) {
            group_str += ",";
            comp_str += "*";
        }
    }
    return fmt::format("/* Val: {val} | Group: {group_str} | Comp: {compute_str} */\n", fmt::arg("val", val),
                       fmt::arg("group_str", group_str), fmt::arg("compute_str", comp_str));
}

} // namespace minigraph
