#include "compiler/codegen.h"
#include "compiler/ir.h"
#include "common/logging.h"
#include "common/timer.h"

#include "compiler/codegen/cpp.h"
#include "common/typedef.h"
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
    if (dep > 0) {
        const VertexSetIR &iter = plan.logical.iter_set.at(dep - 1);
        if (execution_.bitmap_region && dep == execution_.bitmap_region->conversion_depth + 1) {
            out += fmt::format("const uint32_t i{0}_pos = bitmap_region ? bitmap_cursor.position() : 0;\n"
                               "const IdType i{0}_id = bitmap_region ? 0 : s{1}[i{0}_idx];\n", dep, iter.id);
            if (execution_.loops.at(dep).read_adjacency)
                out += fmt::format("VertexSet i{0}_adj = bitmap_region ? VertexSet{{}} : graph->N(i{0}_id);\n", dep);
            return out;
        }
        out += fmt::format("const IdType i{dep}_id = s{iter_id}[i{dep}_idx];\n", fmt::arg("dep", dep),
                           fmt::arg("iter_id", iter.id));
    }

    if (execution_.loops.at(dep).read_adjacency) {
        if (dep > 0)
            out += gen_indent(dep);
        out += fmt::format("VertexSet i{dep}_adj = graph->N(i{dep}_id);\n", fmt::arg("dep", dep));
    }
    return out;
}

std::string CppCodegen::emit_iter(const PlanIR &plan, int dep) {
    if (dep >= plan.logical.p_size - 2) {
        return "";
    } else {
        const auto &iter_set = plan.logical.iter_set.at(dep);
        if (execution_.bitmap_region && dep == execution_.bitmap_region->conversion_depth) {
            const auto &region = *execution_.bitmap_region;
            const auto input = std::find(region.live_ins.begin(), region.live_ins.end(), region.iterator_set)
                               - region.live_ins.begin();
            return fmt::format("auto bitmap_cursor = bitmap_region ? bitmap_region->local_cursor({0}) : BitmapLocalCursor{{}};\n"
                               "for (size_t i{1}_idx = 0; (bitmap_region ? bitmap_cursor.valid() : i{1}_idx < s{2}.size()); "
                               "++i{1}_idx, bitmap_cursor.advance()) {{ // bitmap local-index loop\n", input, dep + 1, iter_set.id);
        }
        return fmt::format("for (size_t i{dep}_idx = 0; i{dep}_idx < s{iter_id}.size(); "
                           "i{dep}_idx++) {left} // loop-{dep} begin\n",
                           fmt::arg("left", "{"), fmt::arg("iter_id", iter_set.id),
                           fmt::arg("dep", dep + 1));
    }
}

namespace {
std::string set_name(SetReference ref) {
    switch (ref.source) {
    case SetSource::Prefix:
        return fmt::format("s{}", ref.id);
    case SetSource::GraphAdjacency:
        return fmt::format("i{}_adj", ref.id);
    case SetSource::MiniGraphAdjacency:
        return fmt::format("m{}_adj", ref.id);
    }
    throw std::logic_error("Unknown set source");
}
std::string vertex_name(const VertexReference &ref) {
    return ref.adjacency ? set_name(*ref.adjacency) + ".vid()" : fmt::format("i{}_id", ref.depth);
}
std::string set_expression(const SetExecution &op) {
    std::string out = set_name(op.input);
    for (size_t i = 0; i < op.steps.size(); ++i) {
        const auto &step = op.steps[i];
        switch (step.opcode) {
        case SetOpcode::Intersect:
            out += ".intersect";
            break;
        // VertexSet::subtract also excludes the right adjacency's owner.
        case SetOpcode::DifferenceExcludingOwner:
            out += ".subtract";
            break;
        case SetOpcode::Bound:
            out += ".bounded";
            break;
        case SetOpcode::Remove:
            out += ".remove";
            break;
        }
        if (op.result == SetResult::Count && i + 1 == op.steps.size())
            out += "_cnt";
        out += "(";
        out += step.rhs ? set_name(*step.rhs) : vertex_name(*step.vertex);
        if (step.upper_bound)
            out += ", " + vertex_name(*step.upper_bound);
        out += ")";
    }
    return out;
}
} // namespace

std::string CppCodegen::emit_op(const PlanIR &, const VertexSetIR &logical) {
    const auto &op = execution_.sets.at(logical.id);
    if (execution_.bitmap_region) {
        const auto &region = *execution_.bitmap_region;
        if (std::find(region.count_ops.begin(), region.count_ops.end(), op.id) != region.count_ops.end()) {
            const auto &step = op.steps.front();
            const auto input = std::find(region.live_ins.begin(), region.live_ins.end(), op.input.id)
                               - region.live_ins.begin();
            const auto expression = fmt::format("bitmap_region ? bitmap_region->count_local({}, i{}_pos, {}, {}) : {}",
                               input, step.rhs->id,
                               step.opcode == SetOpcode::DifferenceExcludingOwner, step.upper_bound.has_value(),
                               set_expression(op));
            if (bitmap_diagnostics_)
                return "{ const auto bitmap_result = " + expression + ";\n"
                       "if (bitmap_region) bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n"
                       "else { bitmap_counters[5].fetch_add(1, std::memory_order_relaxed); "
                       "bitmap_counters[6].fetch_add(bitmap_result, std::memory_order_relaxed); }\n"
                       "counter += bitmap_result; }\n";
            return "counter += " + expression + ";\n";
        }
    }
    std::string out =
        op.result == SetResult::Count ? "counter += " : fmt::format("VertexSet s{} = ", op.id);
    out += set_expression(op) + ";\n";
    if (op.guard_empty)
        out += gen_indent(op.depth) + fmt::format("if (s{}.size() == 0) continue;\n", op.id);
    if (op.result == SetResult::MaterializeThenCount)
        out += fmt::format("counter += s{}.size();\n", op.id);
    return out;
}

std::string CppCodegen::emit_bitmap_build(int dep) {
    if (!execution_.bitmap_region)
        return "";
    const auto &region = *execution_.bitmap_region;
    if (dep != region.entry_depth && dep != region.conversion_depth)
        return "";
    std::string inputs;
    for (int id : region.live_ins) {
        if (!inputs.empty()) inputs += ", ";
        inputs += fmt::format("&s{}", id);
    }
    if (dep == region.conversion_depth) {
        std::string out = gen_indent(dep) + fmt::format(
            "if (bitmap_region) {{ const VertexSet* bitmap_inputs[] = {{{}}}; "
            "bitmap_region->bind_inputs(bitmap_inputs, {}); }}\n", inputs, region.live_ins.size());
        if (bitmap_diagnostics_)
            out += gen_indent(dep) + fmt::format(
                "if (bitmap_region) bitmap_counters[2].fetch_add({}, std::memory_order_relaxed);\n",
                region.live_ins.size());
        return out;
    }
    std::string out = gen_indent(dep) + fmt::format(
        "auto bitmap_neighbors = graph->N(i{0}_id);\n"
        "auto bitmap_region = BitmapCountRegion::build(*graph, i{0}_id, bitmap_neighbors, "
        "bitmap_neighbors, {1}); // bitmap-region build once\n", region.anchor_depth, region.live_ins.size());
    if (bitmap_diagnostics_)
        out += gen_indent(dep) + fmt::format(
            "if (bitmap_region) {{ bitmap_counters[0].fetch_add(1, std::memory_order_relaxed); "
            "bitmap_counters[1].fetch_add(bitmap_region->row_count(), std::memory_order_relaxed); }}\n");
    return out;
}

std::string CppCodegen::gen_mg_type(const PlanIR &, const MiniGraphIR &mg) {
    return execution_.minigraphs.at(mg.id).eager ? "MiniGraphEager" : "MiniGraphType";
}

std::string CppCodegen::emit_mg_init(const PlanIR &plan, const MiniGraphIR &mg) {
    const auto &physical = execution_.minigraphs.at(mg.id);
    return fmt::format("{} m{}({},{});\n", gen_mg_type(plan, mg), mg.id, physical.bounded,
                       physical.parallel);
}

std::string CppCodegen::emit_mg_adj(const PlanIR &plan, int dep, int indent_dep) {
    if (dep == 0)
        return "";
    std::string indent = (indent_dep == -1) ? gen_indent(dep) : gen_indent_tbb(dep);
    const VertexSetIR &iter = plan.logical.iter_set.at(dep - 1);
    std::string out;
    for (const auto &mg : plan.auxiliary.mg_used.at(dep)) {
        const bool same_address = skip_build_indices(plan, mg, iter);

        if (same_address) {
            out += indent;
            out += fmt::format("VertexSet m{mg_id}_adj = m{mg_id}.N(i{dep}_idx);\n",
                               fmt::arg("mg_id", mg.id), fmt::arg("dep", dep));
        } else {
            std::string v_idx = fmt::format("m{mg_id}_s{iter_id}[i{dep}_idx]", fmt::arg("mg_id", mg.id),
                                            fmt::arg("iter_id", iter.id), fmt::arg("dep", dep));
            out += indent;
            out += fmt::format("VertexSet m{mg_id}_adj = m{mg_id}.N({v_idx});\n",
                               fmt::arg("mg_id", mg.id), fmt::arg("v_idx", v_idx));
        }
    }
    return out;
}

bool CppCodegen::skip_build_indices(const PlanIR &, const MiniGraphIR &mg, const VertexSetIR &iter) {
    return execution_.minigraphs.at(mg.id).direct_indices.at(iter.id);
}

std::string CppCodegen::emit_mg_indice(const PlanIR &plan, const MiniGraphIR &mg, int dep) {
    const VertexSetIR &iter = plan.logical.iter_set.at(dep);
    if (skip_build_indices(plan, mg, iter))
        return fmt::format("//skip building indices for m{mg_id} because they can "
                           "be obtained directly\n",
                           fmt::arg("mg_id", mg.id)); // skip
    else
        return fmt::format("auto m{mg_id}_s{iter_id} = m{mg_id}.indices(s{iter_id});\n",
                           fmt::arg("mg_id", mg.id), fmt::arg("iter_id", iter.id));
};

std::string CppCodegen::emit_mg_build(const PlanIR &, const MiniGraphIR &logical) {
    const auto &mg = execution_.minigraphs.at(logical.id);
    std::string out;
    if (mg.estimate_reuse) {
        out += fmt::format("double m{}_factor = 0;\n", mg.id);
        for (const auto &estimate : mg.reuse) {
            std::string visits = fmt::format("s{}.size()", estimate.initial_set);
            for (const auto &factor : estimate.factors) {
                if (factor.set_id)
                    visits += fmt::format(" * s{}.size()", *factor.set_id);
                visits += fmt::format(" * {}", factor.scale);
            }
            out += gen_indent(mg.depth) +
                   fmt::format("m{}_factor += {} * {};\n", mg.id, visits, estimate.uses);
        }
        out +=
            gen_indent(mg.depth) + fmt::format("m{}.set_reuse_multiplier(m{}_factor); ", mg.id, mg.id);
    }
    out += fmt::format("m{}.build(", mg.id);
    if (mg.parent)
        out += fmt::format("&m{}, ", *mg.parent);
    return out + fmt::format("s{}, s{}, s{});\n", mg.vertices, mg.intersect, mg.iter);
}

std::string CppCodegen::emit_mg_op(const PlanIR &plan, const VertexSetIR &op) {
    return emit_op(plan, op);
}

std::string CppCodegen::emit_iep(const PlanIR &, size_t group_id) {
    const auto &term = execution_.iep.at(group_id);
    std::string out = fmt::format("counter += {}ll", term.coefficient);
    for (const auto &factor : term.factors) {
        out += fmt::format(" * s{}", factor.at(0));
        if (factor.size() == 1)
            out += ".size()";
        for (size_t i = 1; i < factor.size(); ++i)
            out += fmt::format(".{}(s{})", i + 1 == factor.size() ? "intersect_cnt" : "intersect",
                               factor[i]);
    }
    return out + ";\n";
}

std::string CppCodegen::gen_comment_iep(const PlanIR &plan, size_t group_id) {
    int val = plan.counting.iep_vals.at(group_id);
    std::string group_str, comp_str;
    const auto &group = plan.counting.iep_groups.at(group_id);
    size_t j = 0;
    for (const auto &set : group) {
        size_t i = 0;
        group_str += "(";
        comp_str += "|";
        for (auto set_id : set) {
            group_str += std::to_string(set_id);
            comp_str += fmt::format("VSet({})", plan.counting.iep_set.at(set_id).id);
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
    return fmt::format("/* Val: {val} | Group: {group_str} | Comp: {compute_str} */\n",
                       fmt::arg("val", val), fmt::arg("group_str", group_str),
                       fmt::arg("compute_str", comp_str));
}

} // namespace minigraph
