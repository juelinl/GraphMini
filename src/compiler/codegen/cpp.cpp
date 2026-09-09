#include "common/types.h"
#include "compiler/codegen.h"
#include "compiler/ir.h"
#include "common/logging.h"
#include "common/timer.h"

#include "compiler/codegen/cpp.h"
#include "compiler/config.h"
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
        if (execution_.bitmap_region && execution_.bitmap_region->full_region &&
            dep == execution_.bitmap_region->entry_depth) {
            if (plan.context.config.parType != ParallelType::OpenMP)
                return emit_bitmap_tasks(plan, dep);
            const auto &region = *execution_.bitmap_region;
            auto slot = [&](int id) { return std::find(region.full_sets.begin(), region.full_sets.end(), id) - region.full_sets.begin(); };
            std::string out = "if (bitmap_region) { // full bitmap region\n";
            out += "auto bitmap_execute = [&](auto bitmap_tag) {\n"
                   "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
            for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) {
                out += fmt::format("for (auto bc{0} = bitmap_region->local_cursor({1}); bc{0}.valid(); bc{0}.advance()) {{ // bitmap local-index loop\n"
                                   "const auto bp{0} = bc{0}.position();\n", depth, slot(plan.logical.iter_set.at(depth-1).id));
                for (const auto &logical : plan.logical.set_ops.at(depth)) {
                    const auto &op = execution_.sets.at(logical.id);
                    const auto &step = op.steps.front();
                    const bool subtract = step.opcode == SetOpcode::DifferenceExcludingOwner;
                    const bool bound_only = step.opcode == SetOpcode::Bound;
                    const bool remove_only = step.opcode == SetOpcode::Remove;
                    const bool bounded = bound_only || step.upper_bound.has_value();
                    if (op.result == SetResult::Count) {
                        out += fmt::format("counter += bitmap_region->count_local<bitmap_words>({}, bp{}, {}, {}, {}, {});\n", slot(op.input.id), depth, subtract, bounded, bound_only || remove_only, remove_only);
                        if (bitmap_diagnostics_) out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
                    } else {
                        if (op.guard_empty || op.result == SetResult::MaterializeThenCount)
                            out += fmt::format("const auto bn{} = ", op.id);
                        out += fmt::format("bitmap_region->materialize_local<bitmap_words>({}, {}, bp{}, {}, {}, {}, {});\n",
                            slot(op.id), slot(op.input.id), depth, subtract, bounded, bound_only || remove_only, remove_only);
                        if (op.guard_empty) out += fmt::format("if (!bn{}) continue;\n", op.id);
                        if (op.result == SetResult::MaterializeThenCount) out += fmt::format("counter += bn{};\n", op.id);
                    }
                }
            }
            for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) out += "}\n";
            out += "};\n"
                   "if (bitmap_region->universe_size() <= 64) bitmap_execute(std::integral_constant<size_t, 1>{});\n"
                   "else if (bitmap_region->universe_size() <= 128) bitmap_execute(std::integral_constant<size_t, 2>{});\n"
                   "else bitmap_execute(std::integral_constant<size_t, 0>{});\n";
            out += "} else {\n";
            return out + fmt::format("for (size_t i{0}_idx = 0; i{0}_idx < s{1}.size(); ++i{0}_idx) {{\n", dep+1, iter_set.id);
        }
        if (execution_.bitmap_region && !execution_.bitmap_region->full_region && dep == execution_.bitmap_region->conversion_depth) {
            const auto &region = *execution_.bitmap_region;
            const auto input = std::find(region.live_ins.begin(), region.live_ins.end(), region.iterator_set)
                               - region.live_ins.begin();
            std::string out = "if (bitmap_region) {\n";
            for (int id : region.count_ops) {
                const auto &op = execution_.sets.at(id);
                const auto index = std::find(region.live_ins.begin(), region.live_ins.end(), op.input.id)
                                   - region.live_ins.begin();
                out += fmt::format("const auto bitmap_count_{} = bitmap_region->counting_view({});\n", id, index);
            }
            out += fmt::format("for (auto bitmap_cursor = bitmap_region->local_cursor({}); "
                               "bitmap_cursor.valid(); bitmap_cursor.advance()) {{ // bitmap local-index loop\n"
                               "const auto bitmap_position = bitmap_cursor.position();\n", input);
            for (int id : region.count_ops) {
                const auto &step = execution_.sets.at(id).steps.front();
                out += fmt::format("counter += bitmap_count_{}.count(bitmap_position, {}, {});\n", id,
                    step.opcode == SetOpcode::DifferenceExcludingOwner, step.upper_bound.has_value());
                if (bitmap_diagnostics_)
                    out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
            }
            out += "}\n} else {\n";
            out += fmt::format("for (size_t i{0}_idx = 0; i{0}_idx < s{1}.size(); ++i{0}_idx) {{\n",
                               dep + 1, iter_set.id);
            return out;
        }
        return fmt::format("for (size_t i{dep}_idx = 0; i{dep}_idx < s{iter_id}.size(); "
                           "i{dep}_idx++) {left} // loop-{dep} begin\n",
                           fmt::arg("left", "{"), fmt::arg("iter_id", iter_set.id),
                           fmt::arg("dep", dep + 1));
    }
}

std::string CppCodegen::emit_bitmap_tasks(const PlanIR &plan, int dep) {
    const auto &region = *execution_.bitmap_region;
    auto slot = [&](int id) { return std::find(region.full_sets.begin(), region.full_sets.end(), id) - region.full_sets.begin(); };
    std::string out = "if (bitmap_region) { // full bitmap region\n"
                      "auto bitmap_execute = [&](auto bitmap_tag) {\n"
                      "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
    for (int depth = plan.logical.p_size - 2; depth > dep; --depth) {
        const auto &loop = execution_.loops.at(depth);
        const auto input = slot(plan.logical.iter_set.at(depth-1).id);
        std::string parallel = "false";
        if (loop.spawn_nested) {
            parallel = "true";
            if (loop.runtime_threshold) {
                int threshold = loop.threshold_factor * loop.average_degree;
                if (loop.cap_threshold) threshold = std::min(threshold, 100);
                parallel = fmt::format("state.input_size({}) > {}", input, threshold);
            }
        }
        out += fmt::format("auto bitmap_level{0} = [&](BitmapCountRegion& state) -> uint64_t {{\n"
                           "return bitmap_for_each(state, {1}, {2}, [&](BitmapCountRegion& task_state, uint32_t bp{0}) -> uint64_t {{ // bitmap local-index loop\n"
                           "auto* bitmap_region = &task_state;\nuint64_t counter = 0;\n", depth, input, parallel);
        for (const auto &logical : plan.logical.set_ops.at(depth)) {
            const auto &op = execution_.sets.at(logical.id);
            const auto &step = op.steps.front();
            const bool subtract = step.opcode == SetOpcode::DifferenceExcludingOwner;
            const bool bound = step.opcode == SetOpcode::Bound;
            const bool remove_only = step.opcode == SetOpcode::Remove;
            const bool bounded = bound || step.upper_bound.has_value();
            if (op.result == SetResult::Count) {
                out += fmt::format("counter += bitmap_region->count_local<bitmap_words>({}, bp{}, {}, {}, {}, {});\n", slot(op.input.id), depth, subtract, bounded, bound || remove_only, remove_only);
                if (bitmap_diagnostics_) out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
            } else {
                out += fmt::format("const auto bn{} = bitmap_region->materialize_local<bitmap_words>({}, {}, bp{}, {}, {}, {}, {});\n",
                    op.id, slot(op.id), slot(op.input.id), depth, subtract, bounded, bound || remove_only, remove_only);
                if (op.guard_empty) out += fmt::format("if (!bn{}) return counter;\n", op.id);
                if (op.result == SetResult::MaterializeThenCount) out += fmt::format("counter += bn{};\n", op.id);
            }
        }
        if (depth < plan.logical.p_size - 2)
            out += fmt::format("counter += bitmap_level{}(task_state);\n", depth+1);
        else
            out += "benchmark_progress->add_bitmap_matches(counter);\n";
        out += fmt::format("return counter;\n}}, bitmap_task_policy, {});\n}};\n", depth-dep-1);
    }
    out += fmt::format("return bitmap_level{}(*bitmap_region);\n", dep+1);
    out += "};\n"
           "if (bitmap_region->universe_size() <= 64) counter.add_without_progress(bitmap_execute(std::integral_constant<size_t, 1>{}));\n"
           "else if (bitmap_region->universe_size() <= 128) counter.add_without_progress(bitmap_execute(std::integral_constant<size_t, 2>{}));\n"
           "else counter.add_without_progress(bitmap_execute(std::integral_constant<size_t, 0>{}));\n"
           "} else {\n";
    out += emit_tbb_call(plan, plan.context.config, dep+1, dep);
    return out + fmt::format("for (size_t i{0}_idx = 0; i{0}_idx < s{1}.size(); ++i{0}_idx) {{\n",
                            dep+1, plan.logical.iter_set.at(dep).id);
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
    // Includes independently emitted array fallback task bodies.
    if (bitmap_diagnostics_ && op.result == SetResult::Count)
        return "{ const auto bitmap_result = " + set_expression(op) + ";\n"
               "bitmap_counters[5].fetch_add(1, std::memory_order_relaxed); "
               "bitmap_counters[6].fetch_add(bitmap_result, std::memory_order_relaxed);\n"
               "counter += bitmap_result; }\n";
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
    const auto &slots = region.full_region ? region.full_sets : region.live_ins;
    const auto &bindings = region.full_region ? region.full_live_ins : region.live_ins;
    if (dep < region.build_depth || dep > region.conversion_depth)
        return "";
    std::string out;
    if (dep == region.build_depth) {
        out = gen_indent(dep) + fmt::format(
            "auto bitmap_neighbors = graph->N(i{0}_id);\n"
            "auto bitmap_rows = BitmapCountRegion::build_rows(*graph, i{0}_id, bitmap_neighbors, "
            "bitmap_neighbors, {1}); // bitmap-region build once per anchor\n", region.anchor_depth, slots.size());
        if (bitmap_diagnostics_)
            out += gen_indent(dep) + fmt::format(
                "if (bitmap_rows) {{ bitmap_counters[0].fetch_add(1, std::memory_order_relaxed); "
                "bitmap_counters[1].fetch_add(bitmap_rows->row_count(), std::memory_order_relaxed); }}\n");
    }
    if (dep == region.entry_depth)
        out += fmt::format("auto bitmap_region = BitmapCountRegion::from_rows(bitmap_rows, {}); // private candidate state\n", slots.size());
    if (dep < region.entry_depth || (region.full_region && dep != region.entry_depth)) return out;
    for (int id : bindings) {
        const auto index = std::find(slots.begin(), slots.end(), id) - slots.begin();
        // SSA set IDs and their definition depths determine the lifetime. An
        // outer set is bound when candidate state is created; inner sets on every
        // definition, after guards, before consumers. Never cache addresses.
        if (dep != std::max(region.entry_depth, execution_.sets.at(id).depth)) continue;
        out += fmt::format("if (bitmap_region) {{ bitmap_region->bind_input({}, s{});", index, id);
        if (bitmap_diagnostics_) out += " bitmap_counters[2].fetch_add(1, std::memory_order_relaxed);";
        out += " }\n";
    }
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
                                                 : fmt::format("i{}_adj", bitmap.anchor_depth);
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
