#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include <algorithm>
#include <fmt/format.h>
#include <string>

namespace minigraph {
namespace {
std::string set_name(int id) { return "s" + std::to_string(id); }
std::string arguments(const std::vector<int> &ids, const std::string &prefix = "s") {
    std::string out;
    for (int id : ids) {
        if (!out.empty()) out += ", ";
        out += prefix + std::to_string(id);
    }
    return out;
}
std::string bound_argument(bool bounded, const std::string &vertex) {
    return bounded ? ", " + vertex : "";
}
} // namespace

// Choose the operation here, rather than passing flags to a runtime slot engine.
// Both backends consume the same verified, single-universe SSA values.
std::string CppCodegen::emit_bitmap_ops(const PlanIR &plan, int depth) {
    const auto vertex = codegen_names::bit_index(depth);
    const auto row = codegen_names::adjacency(depth);
    std::string out;
    bool row_declared = false;
    for (const auto &logical : plan.logical.set_ops.at(depth)) {
        const auto &op = execution_.sets.at(logical.id);
        const auto &step = op.steps.front();
        const auto source = set_name(op.input.id), destination = set_name(op.id);
        const bool count = op.result == SetResult::Count;
        const bool binary = step.opcode == SetOpcode::Intersect || step.opcode == SetOpcode::DifferenceExcludingOwner;
        if (binary && !row_declared) {
            out += fmt::format("const auto {} = bitgraph.local_row({});\n", row, vertex);
            row_declared = true;
        }
        std::string method, args;
        switch (step.opcode) {
        case SetOpcode::Intersect:
            method = count ? "intersection_count" : "assign_intersection";
            args = row + bound_argument(step.upper_bound.has_value(), vertex);
            break;
        case SetOpcode::DifferenceExcludingOwner:
            method = count ? "subtraction_count" : "assign_subtraction";
            args = row + ", " + vertex + bound_argument(step.upper_bound.has_value(), vertex);
            break;
        case SetOpcode::Bound:
            method = count ? "bounded_count" : "assign_bounded";
            args = vertex;
            break;
        case SetOpcode::Remove:
            method = count ? "removed_count" : "assign_removed";
            args = vertex + bound_argument(step.upper_bound.has_value(), vertex);
            break;
        default: throw std::logic_error("Unsupported explicit bitmap operation");
        }
        if (count) {
            out += fmt::format("counter += {}.{}<bitmap_words>({});\n", source, method, args);
            if (bitmap_diagnostics_) out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
        } else {
            out += fmt::format("{}.{}<bitmap_words>({}, {});\n", destination, method, source, args);
            if (op.guard_empty) out += fmt::format("if (!{}.count()) continue;\n", destination);
            if (op.result == SetResult::MaterializeThenCount)
                out += fmt::format("counter += {}.count();\n", destination);
        }
    }
    return out;
}

std::string CppCodegen::emit_bitmap_outputs(int depth) {
    std::string out;
    for (int id : execution_.bitmap_region->loop_outputs.at(depth))
        out += fmt::format("Bitmap s{}(bitgraph.universe());\n", id);
    return out;
}

std::string CppCodegen::emit_bitmap_iter(const PlanIR &plan, int dep) {
    if (!execution_.bitmap_region) return "";
    const auto &region = *execution_.bitmap_region;
    const auto &iter_set = plan.logical.iter_set.at(dep);
    if (region.full_region && dep == region.entry_depth) {
        if (plan.context.config.parType != ParallelType::OpenMP) return emit_bitmap_tasks(plan, dep);
        std::string out = "if (bitmap_rows) { // full bitmap region\nconst auto& bitgraph = *bitmap_rows;\n";
        for (int id : region.full_live_ins) out += fmt::format("const Bitmap& s{0} = *bitmap_s{0};\n", id);
        out += "auto bitmap_execute = [&](auto bitmap_tag) {\nconstexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) {
            out += emit_bitmap_outputs(depth);
            out += fmt::format("for (auto bc{0} = s{1}.local_cursor(); bc{0}.valid(); bc{0}.advance()) {{ "
                               "// bitmap local-index loop\nconst auto {2} = bc{0}.position();\n",
                               depth, plan.logical.iter_set.at(depth - 1).id, codegen_names::bit_index(depth));
            out += emit_bitmap_ops(plan, depth);
        }
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) out += "}\n";
        out += "};\ndispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute);\n} else {\n";
        return out + fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n",
                                 codegen_names::index(dep + 1), iter_set.id);
    }
    if (!region.full_region && dep == region.conversion_depth) {
        std::string out = "if (bitmap_rows) { // bitmap terminal region\nconst auto& bitgraph = *bitmap_rows;\n";
        for (int id : region.live_ins) out += fmt::format("const Bitmap& s{0} = *bitmap_s{0};\n", id);
        out += "constexpr size_t bitmap_words = 0;\n";
        out += fmt::format("for (auto bitmap_cursor = s{}.local_cursor(); bitmap_cursor.valid(); bitmap_cursor.advance()) {{ "
                           "// bitmap local-index loop\nconst auto {} = bitmap_cursor.position();\n",
                           region.iterator_set, codegen_names::bit_index(dep + 1));
        out += emit_bitmap_ops(plan, dep + 1);
        out += "}\n} else {\n";
        return out + fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n",
                                 codegen_names::index(dep + 1), iter_set.id);
    }
    return "";
}

std::string CppCodegen::emit_bitmap_tasks(const PlanIR &plan, int dep) {
    const auto &region = *execution_.bitmap_region;
    std::string out = "if (bitmap_rows) { // full bitmap region\n"
                      "const auto& bitgraph = *bitmap_rows;\nauto& progress = query.progress;\n"
                      "auto bitmap_execute = [&](auto bitmap_tag) {\n"
                      "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
    for (int depth = plan.logical.p_size - 2; depth > dep; --depth) {
        const auto &loop = execution_.loops.at(depth);
        const int input = plan.logical.iter_set.at(depth - 1).id;
        const auto &inputs = region.loop_inputs.at(depth);
        std::string parallel = loop.spawn_nested ? "true" : "false";
        if (loop.spawn_nested && loop.runtime_threshold) {
            int threshold = loop.threshold_factor * loop.average_degree;
            if (loop.cap_threshold) threshold = std::min(threshold, 100);
            parallel = fmt::format("input_s{}.count() > {}", input, threshold);
        }
        out += fmt::format("auto bitmap_level{} = [&]({}) -> uint64_t {{\n", depth,
                           arguments(inputs, "const Bitmap& input_s"));
        out += fmt::format("return bitmap_for_each(input_s{}, {}, [&](size_t begin, size_t end, bool parallel_task) -> uint64_t {{ // private task range\n",
                           input, parallel);
        for (int id : inputs)
            out += fmt::format("BitmapTaskInput task_s{0}(input_s{0}, parallel_task, bitmap_task_policy);\n"
                               "const Bitmap& s{0} = task_s{0}.get();\n", id);
        out += emit_bitmap_outputs(depth);
        out += fmt::format("uint64_t counter = 0;\n"
                           "for (auto bc{0} = s{1}.local_cursor(begin, end); bc{0}.valid(); bc{0}.advance()) {{ "
                           "// bitmap local-index loop\nconst auto {2} = bc{0}.position();\n",
                           depth, input, codegen_names::bit_index(depth));
        if (depth == plan.logical.p_size - 2) out += "const uint64_t previous_count = counter;\n";
        out += emit_bitmap_ops(plan, depth);
        if (depth < plan.logical.p_size - 2)
            out += fmt::format("counter += bitmap_level{}({});\n", depth + 1, arguments(region.loop_inputs.at(depth + 1)));
        else out += "progress.add_bitmap_matches(counter - previous_count);\n";
        out += "}\n";
        out += fmt::format("return counter;\n}}, bitmap_task_policy, {});\n}};\n", depth - dep - 1);
    }
    out += fmt::format("return bitmap_level{}({});\n", dep + 1, arguments(region.loop_inputs.at(dep + 1), "*bitmap_s"));
    out += "};\ncounter.add_without_progress(dispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute));\n} else {\n";
    out += emit_tbb_call(plan, plan.context.config, dep + 1);
    return out + fmt::format("for (size_t {0} = 0; {0} < s{1}.size(); ++{0}) {{\n", codegen_names::index(dep + 1),
                             plan.logical.iter_set.at(dep).id);
}

std::string CppCodegen::emit_bitmap_build(int dep) {
    if (!execution_.bitmap_region) return "";
    const auto &region = *execution_.bitmap_region;
    if (dep < region.build_depth || dep > region.conversion_depth) return "";
    std::string out;
    if (dep == region.build_depth) {
        const bool reuse_adjacency = dep == region.anchor_depth && execution_.loops.at(dep).read_adjacency;
        const auto neighbors = reuse_adjacency ? codegen_names::adjacency(dep) : "bitmap_neighbors";
        if (!reuse_adjacency)
            out += fmt::format("auto bitmap_neighbors = {}->N({});\n", graph_name_, codegen_names::vertex(region.anchor_depth));
        out += fmt::format("auto bitmap_rows = BitGraph::build(*{3}, {0}, {1}, {1}, {2}); "
                           "// bitmap-region build once per anchor\n",
                           codegen_names::vertex(region.anchor_depth), neighbors, region.slots.size(), graph_name_);
        if (bitmap_diagnostics_)
            out += "if (bitmap_rows) { bitmap_counters[0].fetch_add(1, std::memory_order_relaxed); "
                   "bitmap_counters[1].fetch_add(bitmap_rows->row_count(), std::memory_order_relaxed); }\n";
    }
    if (dep == region.entry_depth) {
        const auto &inputs = region.full_region ? region.full_live_ins : region.live_ins;
        for (int id : inputs) out += fmt::format("std::optional<Bitmap> bitmap_s{};\n", id);
    }
    if (region.projection_pair && dep == region.entry_depth) {
        const auto &pair = *region.projection_pair;
        out += fmt::format("if (bitmap_rows) {{ // shared bounded neighborhood projection\n"
                           "const auto& universe = bitmap_rows->universe();\n"
                           "const auto local_bound = universe.lower_bound({0});\n"
                           "const auto local_row = bitmap_rows->row({0});\n"
                           "bitmap_s{1}.emplace(universe);\nbitmap_s{2}.emplace(universe);\n"
                           "bitmap_s{1}->assign_neighbors({3}.data(), {3}.size(), {0});\n"
                           "bitmap_s{2}->assign_subtraction(*bitmap_s{1}, local_row, local_bound, local_bound);\n"
                           "bitmap_s{1}->assign_intersection(*bitmap_s{1}, local_row, local_bound);\n",
                           codegen_names::vertex(pair.local_depth), pair.positive, pair.negative,
                           codegen_names::adjacency(pair.external_depth));
        for (const auto &binding : region.bindings)
            if (execution_.sets.at(binding.set_id).guard_empty)
                out += fmt::format("if (!bitmap_s{}->count()) continue;\n", binding.set_id);
        return out + "}\n";
    }
    for (const auto &binding : region.bindings) {
        if (dep != binding.depth) continue;
        out += fmt::format("if (bitmap_rows) {{ bitmap_s{0}.emplace(Bitmap::from_sorted(bitmap_rows->universe(), s{0}.data(), s{0}.size()));",
                           binding.set_id);
        if (bitmap_diagnostics_) out += " bitmap_counters[2].fetch_add(1, std::memory_order_relaxed);";
        out += " }\n";
    }
    return out;
}
} // namespace minigraph
