#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include <algorithm>
#include <fmt/format.h>
#include <string>

namespace minigraph {
// Full regions have already been verified to contain single-step operations in
// one universe. Backends supply state access and empty-result control flow;
// task boundaries, iteration, reduction and progress remain at the call sites.
std::string CppCodegen::emit_bitmap_ops(const PlanIR &plan, int depth, const BitmapEmission &target) {
    const auto &slots = execution_.bitmap_region->slots;
    const auto slot = [&](int id) { return slots.at(id); };
    const auto vertex = codegen_names::bit_index(depth);
    std::string out;
    for (const auto &logical : plan.logical.set_ops.at(depth)) {
        const auto &op = execution_.sets.at(logical.id);
        const auto &step = op.steps.front();
        const bool subtract = step.opcode == SetOpcode::DifferenceExcludingOwner;
        const bool bound = step.opcode == SetOpcode::Bound;
        const bool remove = step.opcode == SetOpcode::Remove;
        const bool bounded = bound || step.upper_bound.has_value();
        if (op.result == SetResult::Count) {
            out += fmt::format("counter += {}count_local<bitmap_words>({}, {}, {}, {}, {}, {});\n", target.state_access,
                               slot(op.input.id), vertex, subtract, bounded, bound || remove, remove);
            if (bitmap_diagnostics_)
                out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
        } else {
            if (op.guard_empty || op.result == SetResult::MaterializeThenCount)
                out += fmt::format("const auto bn{} = ", op.id);
            out += fmt::format("{}materialize_local<bitmap_words>({}, {}, {}, {}, {}, {}, {});\n", target.state_access,
                               slot(op.id), slot(op.input.id), vertex, subtract, bounded, bound || remove, remove);
            if (op.guard_empty)
                out += fmt::format("if (!bn{}) {}\n", op.id, target.empty_action);
            if (op.result == SetResult::MaterializeThenCount)
                out += fmt::format("counter += bn{};\n", op.id);
        }
    }
    return out;
}

// Return an empty string outside a bitmap boundary so the caller emits its
// ordinary array loop. Bitmap branches retain their own array fallback entry.
std::string CppCodegen::emit_bitmap_iter(const PlanIR &plan, int dep) {
    if (!execution_.bitmap_region)
        return "";
    const auto &iter_set = plan.logical.iter_set.at(dep);
    const auto &region = *execution_.bitmap_region;
    if (region.full_region && dep == region.entry_depth) {
        if (plan.context.config.parType != ParallelType::OpenMP)
            return emit_bitmap_tasks(plan, dep);
        const auto slot = [&](int id) { return region.slots.at(id); };
        std::string out = "if (bitmap_region) { // full bitmap region\n";
        out += "auto bitmap_execute = [&](auto bitmap_tag) {\n"
               "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) {
            out += fmt::format("for (auto bc{0} = bitmap_region->local_cursor({1}); bc{0}.valid(); bc{0}.advance()) {{ "
                               "// bitmap local-index loop\n"
                               "const auto {2} = bc{0}.position();\n",
                               depth, slot(plan.logical.iter_set.at(depth - 1).id), codegen_names::bit_index(depth));
            out += emit_bitmap_ops(plan, depth, {"bitmap_region->", "continue;"});
        }
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth)
            out += "}\n";
        out += "};\n"
               "dispatch_bitmap_words(bitmap_region->universe_size(), bitmap_execute);\n";
        out += "} else {\n";
        return out + fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n", codegen_names::index(dep + 1),
                                 iter_set.id);
    }
    if (!region.full_region && dep == region.conversion_depth) {
        const auto input = region.slots.at(region.iterator_set);
        std::string out = "if (bitmap_region) {\n";
        for (int id : region.count_ops) {
            const auto &op = execution_.sets.at(id);
            const auto index = region.slots.at(op.input.id);
            out += fmt::format("const auto bitmap_count_{} = bitmap_region->counting_view({});\n", id, index);
        }
        out += fmt::format("for (auto bitmap_cursor = bitmap_region->local_cursor({0}); "
                           "bitmap_cursor.valid(); bitmap_cursor.advance()) {{ // bitmap local-index loop\n"
                           "const auto {1} = bitmap_cursor.position();\n",
                           input, codegen_names::bit_index(dep + 1));
        for (int id : region.count_ops) {
            const auto &step = execution_.sets.at(id).steps.front();
            out += fmt::format("counter += bitmap_count_{}.count({}, {}, {});\n", id, codegen_names::bit_index(dep + 1),
                               step.opcode == SetOpcode::DifferenceExcludingOwner, step.upper_bound.has_value());
            if (bitmap_diagnostics_)
                out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
        }
        out += "}\n} else {\n";
        out += fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n", codegen_names::index(dep + 1),
                           iter_set.id);
        return out;
    }
    return "";
}

std::string CppCodegen::emit_bitmap_tasks(const PlanIR &plan, int dep) {
    const auto &region = *execution_.bitmap_region;
    const auto slot = [&](int id) { return region.slots.at(id); };
    std::string out = "if (bitmap_region) { // full bitmap region\n"
                      "auto& progress = query.progress;\n"
                      "auto bitmap_execute = [&](auto bitmap_tag) {\n"
                      "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
    for (int depth = plan.logical.p_size - 2; depth > dep; --depth) {
        const auto &loop = execution_.loops.at(depth);
        const auto input = slot(plan.logical.iter_set.at(depth - 1).id);
        std::string parallel = "false";
        if (loop.spawn_nested) {
            parallel = "true";
            if (loop.runtime_threshold) {
                int threshold = loop.threshold_factor * loop.average_degree;
                if (loop.cap_threshold)
                    threshold = std::min(threshold, 100);
                parallel = fmt::format("state.input_size({}) > {}", input, threshold);
            }
        }
        out += fmt::format("auto bitmap_level{0} = [&](BitmapCountRegion& state) -> uint64_t {{\n"
                           "return bitmap_for_each(state, {1}, {2}, [&](BitmapCountRegion& task_state, uint32_t {3}) "
                           "-> uint64_t {{ // bitmap local-index loop\n"
                           "uint64_t counter = 0;\n",
                           depth, input, parallel, codegen_names::bit_index(depth));
        out += emit_bitmap_ops(plan, depth, {"task_state.", "return counter;"});
        if (depth < plan.logical.p_size - 2)
            out += fmt::format("counter += bitmap_level{}(task_state);\n", depth + 1);
        else
            out += "progress.add_bitmap_matches(counter);\n";
        out += fmt::format("return counter;\n}}, bitmap_task_policy, {});\n}};\n", depth - dep - 1);
    }
    out += fmt::format("return bitmap_level{}(*bitmap_region);\n", dep + 1);
    out += "};\n"
           "counter.add_without_progress(dispatch_bitmap_words(bitmap_region->universe_size(), bitmap_execute));\n"
           "} else {\n";
    out += emit_tbb_call(plan, plan.context.config, dep + 1);
    return out + fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n", codegen_names::index(dep + 1),
                             plan.logical.iter_set.at(dep).id);
}

std::string CppCodegen::emit_bitmap_build(int dep) {
    if (!execution_.bitmap_region)
        return "";
    const auto &region = *execution_.bitmap_region;
    const auto &slots = region.slots;
    if (dep < region.build_depth || dep > region.conversion_depth)
        return "";
    std::string out;
    if (dep == region.build_depth) {
        // Reuse the full, non-owning adjacency view only when it is available
        // in this scope. Prefix views may be bounded and are not interchangeable.
        const bool reuse_adjacency = dep == region.anchor_depth && execution_.loops.at(dep).read_adjacency;
        const auto neighbors = reuse_adjacency ? codegen_names::adjacency(dep) : "bitmap_neighbors";
        if (!reuse_adjacency)
            out += fmt::format("auto bitmap_neighbors = {}->N({});\n", graph_name_,
                               codegen_names::vertex(region.anchor_depth));
        out += fmt::format("auto bitmap_rows = BitmapCountRegion::build_rows(*{3}, {0}, {1}, "
                           "{1}, {2}); // bitmap-region build once per anchor\n",
                           codegen_names::vertex(region.anchor_depth), neighbors, slots.size(), graph_name_);
        if (bitmap_diagnostics_)
            out +=
                fmt::format("if (bitmap_rows) {{ bitmap_counters[0].fetch_add(1, std::memory_order_relaxed); "
                            "bitmap_counters[1].fetch_add(bitmap_rows->row_count(), std::memory_order_relaxed); }}\n");
    }
    if (dep == region.entry_depth)
        out += fmt::format(
            "auto bitmap_region = BitmapCountRegion::from_rows(bitmap_rows, {}); // private candidate state\n",
            slots.size());
    if (region.projection_pair && dep == region.entry_depth) {
        const auto &pair = *region.projection_pair;
        const auto slot = [&](int id) { return slots.at(id); };
        out += fmt::format("if (bitmap_region) {{ // shared bounded neighborhood projection\n"
                           "bitmap_region->bind_projected_partition({}, {}, {}, {});\n",
                           slot(pair.positive), slot(pair.negative), codegen_names::adjacency(pair.external_depth),
                           codegen_names::vertex(pair.local_depth));
        for (const auto &binding : region.bindings)
            if (execution_.sets.at(binding.set_id).guard_empty)
                out += fmt::format("if (!bitmap_region->input_size({})) continue;\n", binding.slot);
        out += "}\n";
        return out;
    }
    for (const auto &binding : region.bindings) {
        if (dep != binding.depth)
            continue;
        out += fmt::format("if (bitmap_region) {{ bitmap_region->bind_input({}, s{});", binding.slot, binding.set_id);
        if (bitmap_diagnostics_)
            out += " bitmap_counters[2].fetch_add(1, std::memory_order_relaxed);";
        out += " }\n";
    }
    return out;
}

} // namespace minigraph
