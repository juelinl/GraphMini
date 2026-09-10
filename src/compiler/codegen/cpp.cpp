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
bool CppCodegen::uses_selected_vertex(int dep) const {
    if (execution_.loops.at(dep).read_adjacency)
        return true;
    const auto selected = [dep](const std::optional<VertexReference> &ref) {
        return ref && !ref->adjacency && ref->depth == dep;
    };
    for (const auto &[id, op] : execution_.sets)
        for (const auto &step : op.steps)
            if (selected(step.vertex) || selected(step.upper_bound))
                return true;
    if (execution_.bitmap_region) {
        const auto &region = *execution_.bitmap_region;
        if (region.anchor_depth == dep ||
            (region.projection_pair && region.projection_pair->local_depth == dep))
            return true;
    }
    return false;
}

std::string CppCodegen::emit_read_adj(const PlanIR &plan, int dep) {
    std::string out;
    if (profiling_) {
        out += fmt::format("ctx.profiler->set_cur_loop({dep});\n", fmt::arg("dep", dep));
    }
    if (dep > 0 && uses_selected_vertex(dep)) {
        const VertexSetIR &iter = plan.logical.iter_set.at(dep - 1);
        out += fmt::format("const IdType {} = s{}[{}];\n", codegen_names::vertex(dep),
                           iter.id, codegen_names::index(dep));
    }

    if (execution_.loops.at(dep).read_adjacency) {
        out += fmt::format("VertexSet {} = {}->N({});\n", codegen_names::adjacency(dep), graph_name_, codegen_names::vertex(dep));
    }
    return out;
}

std::string CppCodegen::emit_iter(const PlanIR &plan, int dep) {
    if (dep >= plan.logical.p_size - 2)
        return "";
    const auto &iter_set = plan.logical.iter_set.at(dep);
    return fmt::format("for (size_t {idx} = 0; {idx} < s{iter_id}.size(); "
                       "{idx}++) {left} // loop-{dep} begin\n",
                       fmt::arg("left", "{"), fmt::arg("iter_id", iter_set.id),
                       fmt::arg("dep", dep + 1), fmt::arg("idx", codegen_names::index(dep + 1)));
}

namespace {
std::string set_name(SetReference ref) {
    switch (ref.source) {
    case SetSource::Prefix:
        return fmt::format("s{}", ref.id);
    case SetSource::GraphAdjacency:
        return codegen_names::adjacency(ref.id);
    case SetSource::MiniGraphAdjacency:
        return fmt::format("m{}_adj", ref.id);
    }
    throw std::logic_error("Unknown set source");
}
std::string vertex_name(const VertexReference &ref) {
    return ref.adjacency ? set_name(*ref.adjacency) + ".vid()" : codegen_names::vertex(ref.depth);
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
    // Includes independently emitted array fallback task bodies.
    if (bitmap_diagnostics_ && op.result == SetResult::Count)
        return "{ const auto bitmap_result = " + set_expression(op) + ";\n"
               "bitmap_counters[5].fetch_add(1, std::memory_order_relaxed); "
               "bitmap_counters[6].fetch_add(bitmap_result, std::memory_order_relaxed);\n"
               "counter += bitmap_result; }\n";
    const bool fallback_only = execution_.bitmap_region && execution_.bitmap_region->projection_pair &&
        std::find(execution_.bitmap_region->projection_pair->fallback_sets.begin(),
                  execution_.bitmap_region->projection_pair->fallback_sets.end(), op.id) !=
                  execution_.bitmap_region->projection_pair->fallback_sets.end();
    if (fallback_only && bitmap_enabled_) return "";
    std::string out = op.result == SetResult::Count ? "counter += " : fmt::format("VertexSet s{} = ", op.id);
    out += set_expression(op) + ";\n";
    if (op.guard_empty)
        out += fmt::format("if (s{}.size() == 0) continue;\n", op.id);
    if (op.result == SetResult::MaterializeThenCount)
        out += fmt::format("counter += s{}.size();\n", op.id);
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

std::string CppCodegen::emit_mg_adj(const PlanIR &plan, int dep) {
    if (dep == 0)
        return "";
    const VertexSetIR &iter = plan.logical.iter_set.at(dep - 1);
    std::string out;
    for (const auto &mg : plan.auxiliary.mg_used.at(dep)) {
        const bool same_address = skip_build_indices(plan, mg, iter);

        if (same_address) {
            out += fmt::format("VertexSet m{mg_id}_adj = m{mg_id}.N({idx});\n",
                               fmt::arg("mg_id", mg.id), fmt::arg("idx", codegen_names::index(dep)));
        } else {
            std::string v_idx = fmt::format("m{mg_id}_s{iter_id}[{idx}]", fmt::arg("mg_id", mg.id),
                                            fmt::arg("iter_id", iter.id), fmt::arg("idx", codegen_names::index(dep)));
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
            out += fmt::format("m{}_factor += {} * {};\n", mg.id, visits, estimate.uses);
        }
        out += fmt::format("m{}.set_reuse_multiplier(m{}_factor); ", mg.id, mg.id);
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
    auto array_count = [](const std::vector<int> &factor) {
        std::string result = fmt::format("s{}", factor.at(0));
        if (factor.size() == 1) result += ".size()";
        for (size_t i = 1; i < factor.size(); ++i)
            result += fmt::format(".{}(s{})", i + 1 == factor.size() ? "intersect_cnt" : "intersect", factor[i]);
        return result;
    };
    std::string out;
    if (execution_.iep_bitmap && group_id == 0) {
        const auto &bitmap = *execution_.iep_bitmap;
        out = "// bitmap-backed IEP: shared-universe factor cardinalities\n";
        const auto universe = bitmap.universe_set ? fmt::format("s{}", *bitmap.universe_set)
                                                 : codegen_names::adjacency(bitmap.anchor_depth);
        out += fmt::format("IEPBitmap iep_bitmap({}, {{", universe);
        for (size_t i = 0; i < bitmap.inputs.size(); ++i)
            out += fmt::format("{}&s{}", i ? ", " : "", bitmap.inputs[i]);
        out += "});\n";
        for (size_t i = 0; i < bitmap.factors.size(); ++i) {
            out += fmt::format("const auto iep_factor{} = iep_bitmap.enabled() ? iep_bitmap.intersection_count({{", i);
            for (size_t j = 0; j < bitmap.factors[i].size(); ++j) {
                auto slot = std::find(bitmap.inputs.begin(), bitmap.inputs.end(), bitmap.factors[i][j]) - bitmap.inputs.begin();
                out += fmt::format("{}{}", j ? ", " : "", slot);
            }
            out += "}) : " + array_count(bitmap.factors[i]) + ";\n";
        }
    }
    out += fmt::format("counter += {}ll", term.coefficient);
    for (const auto &factor : term.factors) {
        auto key = factor;
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        if (execution_.iep_bitmap && key.size() > 1) {
            const auto &factors = execution_.iep_bitmap->factors;
            out += fmt::format(" * iep_factor{}", std::find(factors.begin(), factors.end(), key) - factors.begin());
        } else out += " * " + array_count(factor);
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
