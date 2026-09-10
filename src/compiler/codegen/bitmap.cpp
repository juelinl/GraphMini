#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include <algorithm>
#include <fmt/format.h>
#include <string>

namespace minigraph {
namespace {
std::string arguments(const std::vector<int> &ids, const std::string &prefix = "b") {
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
        const auto source = codegen_names::bitmap(op.input.id), destination = codegen_names::bitmap(op.id);
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
            // In deferred mode, ordinary intermediates carry safe capacity bounds;
            // immediately consumed counts still use fused population counting.
            const bool count_materialized = !plan.context.config.bitmapDeferredCounts ||
                                            op.result == SetResult::MaterializeThenCount;
            out += fmt::format("{}.{}<bitmap_words, {}>({}, {});\n", destination, method,
                               count_materialized ? "true" : "false", source, args);
            if (op.guard_empty) out += fmt::format("if ({}.empty()) continue;\n", destination);
            if (op.result == SetResult::MaterializeThenCount)
                out += fmt::format("counter += {}.count();\n", destination);
        }
    }
    return out;
}

std::string CppCodegen::emit_bitmap_outputs(int depth) {
    std::string out;
    for (int id : execution_.bitmap_region->loop_outputs.at(depth))
        out += fmt::format("Bitmap b{}(bitgraph.universe());\n", id);
    return out;
}

// Called only inside the successful construction continuation.
std::string CppCodegen::emit_bitmap_iter(const PlanIR &plan, int dep) {
    const auto &region = *execution_.bitmap_region;
    if (region.full_region) {
        if (plan.context.config.parType != ParallelType::OpenMP) return emit_bitmap_tasks(plan, dep);
        std::string out = "{ // full bitmap region\n";
        out += "auto bitmap_execute = [&](auto bitmap_tag) {\nconstexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) {
            out += emit_bitmap_outputs(depth);
            out += fmt::format("for (auto bc{0} = b{1}.local_cursor(); bc{0}.valid(); bc{0}.advance()) {{ "
                               "// bitmap local-index loop\nconst auto {2} = bc{0}.position();\n",
                               depth, plan.logical.iter_set.at(depth - 1).id, codegen_names::bit_index(depth));
            out += emit_bitmap_ops(plan, depth);
        }
        for (int depth = dep + 1; depth <= plan.logical.p_size - 2; ++depth) out += "}\n";
        return out + "};\ndispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute);\n} // end bitmap region\n";
    }
    std::string out = "{ // bitmap terminal region\nconstexpr size_t bitmap_words = 0;\n";
    out += fmt::format("for (auto bitmap_cursor = b{}.local_cursor(); bitmap_cursor.valid(); bitmap_cursor.advance()) {{ "
                       "// bitmap local-index loop\nconst auto {} = bitmap_cursor.position();\n",
                       region.iterator_set, codegen_names::bit_index(dep + 1));
    out += emit_bitmap_ops(plan, dep + 1);
    return out + "}\n} // end bitmap region\n";
}

// Emit innermost first so each level can directly instantiate its successor.
// Fields are references only; range-local scratch must never become task state.
std::string CppCodegen::emit_bitmap_levels(const PlanIR &plan) {
    if (!execution_.bitmap_region || !execution_.bitmap_region->full_region) return "";
    const auto &region = *execution_.bitmap_region;
    std::string out = "// Bitmap level definitions: borrowed inputs; invocation-local scratch.\n";
    for (int depth = plan.logical.p_size - 2; depth > region.entry_depth; --depth) {
        const auto name = codegen_names::bit_level(depth);
        const auto &inputs = region.loop_inputs.at(depth);
        const auto &loop = execution_.loops.at(depth);
        const int input = plan.logical.iter_set.at(depth - 1).id;
        out += fmt::format("template<size_t bitmap_words>\nclass {} {{\n", name);
        out += "const QueryContext& query;\nconst BitGraph& bitgraph;\nconst BitmapTaskPolicy& policy;\n";
        for (int id : inputs) out += fmt::format("const Bitmap& input_b{};\n", id);
        out += fmt::format("public:\n{}(const QueryContext& query, const BitGraph& bitgraph, "
                           "const BitmapTaskPolicy& policy, {})\n"
                           ": query(query), bitgraph(bitgraph), policy(policy)",
                           name, arguments(inputs, "const Bitmap& input_b"));
        for (int id : inputs) out += fmt::format(", input_b{0}(input_b{0})", id);
        out += " {}\n";
        std::string parallel = loop.spawn_nested ? "true" : "false";
        std::string threshold = "0";
        if (loop.spawn_nested && loop.runtime_threshold) {
            threshold = fmt::format("query.nested_thresholds[{}]", depth);
        }
        out += fmt::format("uint64_t operator()() const {{\n"
                           "return bitmap_for_each(input_b{}, {}, *this, policy, {}, {});\n}}\n",
                           input, parallel, depth - region.entry_depth - 1, threshold);
        out += fmt::format("template<class Cursor>\nuint64_t operator()(Cursor bc{}, bool parallel_task) const {{\n", depth);
        for (int id : inputs)
            out += fmt::format("BitmapTaskInput task_b{0}(input_b{0}, parallel_task, policy);\n"
                               "const Bitmap& b{0} = task_b{0}.get();\n", id);
        out += emit_bitmap_outputs(depth);
        out += fmt::format("uint64_t counter = 0;\n"
                           "for (; bc{0}.valid(); bc{0}.advance()) {{ "
                           "// bitmap local-index loop\nconst auto {2} = bc{0}.position();\n",
                           depth, input, codegen_names::bit_index(depth));
        if (depth == plan.logical.p_size - 2) out += "const uint64_t previous_count = counter;\n";
        out += emit_bitmap_ops(plan, depth);
        if (depth < plan.logical.p_size - 2)
            out += fmt::format("counter += {}<bitmap_words>(query, bitgraph, policy, {})();\n",
                               codegen_names::bit_level(depth + 1), arguments(region.loop_inputs.at(depth + 1)));
        else out += "query.progress.add_bitmap_matches(counter - previous_count);\n";
        out += "}\nreturn counter;\n}\n};\n";
    }
    return out + "// End bitmap level definitions.\n";
}

std::string CppCodegen::emit_bitmap_tasks(const PlanIR &plan, int dep) {
    const auto &region = *execution_.bitmap_region;
    std::string out = "{ // full bitmap region\n"
                      "auto bitmap_execute = [&](auto bitmap_tag) {\n"
                      "constexpr size_t bitmap_words = decltype(bitmap_tag)::value;\n";
    out += fmt::format("return {}<bitmap_words>(query, bitgraph, bitmap_task_policy, {})();\n",
                       codegen_names::bit_level(dep + 1), arguments(region.loop_inputs.at(dep + 1)));
    return out + "};\ncounter.add_without_progress(dispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute));\n"
                 "} // end bitmap region\n";
}

std::string CppCodegen::emit_bitmap_build(int dep) {
    if (!execution_.bitmap_region || dep != execution_.bitmap_region->build_depth) return "";
    const auto &region = *execution_.bitmap_region;
    const bool reuse_adjacency = dep == region.anchor_depth && execution_.loops.at(dep).read_adjacency;
    const auto neighbors = reuse_adjacency ? codegen_names::adjacency(dep) : "bitmap_neighbors";
    std::string out;
    if (!reuse_adjacency)
        out += fmt::format("auto bitmap_neighbors = {}->N({});\n", graph_name_, codegen_names::vertex(region.anchor_depth));
    out += fmt::format("auto bitmap_rows = BitGraph::build(*{3}, {0}, {1}, {1}, {2}); "
                       "// bitmap-region build once per anchor\n",
                       codegen_names::vertex(region.anchor_depth), neighbors, region.slots.size(), graph_name_);
    return out;
}

std::string CppCodegen::emit_bitmap_bindings(int dep) {
    const auto &region = *execution_.bitmap_region;
    std::string out;
    if (region.projection_pair && dep == region.entry_depth) {
        const auto &pair = *region.projection_pair;
        out += fmt::format("// shared bounded neighborhood projection\n"
                           "const auto& universe = bitgraph.universe();\n"
                           "const auto local_bound = universe.lower_bound({0});\n"
                           "const auto local_row = bitgraph.row({0});\n"
                           "Bitmap b{1}(universe);\nBitmap b{2}(universe);\n"
                           "b{1}.assign_neighbors({3}.data(), {3}.size(), {0});\n"
                           "b{2}.assign_subtraction(b{1}, local_row, local_bound, local_bound);\n"
                           "b{1}.assign_intersection(b{1}, local_row, local_bound);\n",
                           codegen_names::vertex(pair.local_depth), pair.positive, pair.negative,
                           codegen_names::adjacency(pair.external_depth));
        for (const auto &binding : region.bindings)
            if (execution_.sets.at(binding.set_id).guard_empty)
                out += fmt::format("if (!b{}.count()) continue;\n", binding.set_id);
        return out;
    }
    for (const auto &binding : region.bindings) {
        if (dep != binding.depth) continue;
        out += fmt::format("Bitmap b{0} = Bitmap::from_sorted(bitgraph.universe(), s{0}.data(), s{0}.size());\n",
                           binding.set_id);
        if (bitmap_diagnostics_) out += "bitmap_counters[2].fetch_add(1, std::memory_order_relaxed);\n";
    }
    return out;
}

// Structured emission keeps the construction decision outside both continuations.
// Recursing here (rather than closing loops in a separate pass) keeps each branch,
// conversion, and its local buffers in the dependency scope recorded by the IR.
std::string CppCodegen::emit_search_body(const PlanIR &plan, const CodeGenConfig &config, int dep) {
    std::string out = emit_read_adj(plan, dep);
    for (const auto &op : plan.logical.set_ops.at(dep)) out += emit_op(plan, op);
    if (dep == plan.logical.p_size - 2) return out;
    if (execution_.bitmap_region && !bitmap_enabled_ && dep == execution_.bitmap_region->build_depth) {
        out += emit_bitmap_build(dep);
        out += "if (bitmap_rows) { // bitmap-enabled continuation\nconst auto& bitgraph = *bitmap_rows;\n";
        if (bitmap_diagnostics_)
            out += "bitmap_counters[0].fetch_add(1, std::memory_order_relaxed);\n"
                   "bitmap_counters[1].fetch_add(bitgraph.row_count(), std::memory_order_relaxed);\n";
        CppCodegen enabled(config, execution_);
        enabled.bitmap_enabled_ = true;
        out += enabled.emit_search_tail(plan, config, dep);
        out += "} else { // array-only continuation\n";
        auto arrays = execution_;
        arrays.bitmap_region.reset();
        CppCodegen fallback(config, arrays);
        // Preserve the former task boundary: no new array task before the
        // depth at which the old bitmap/array region dispatched its fallback.
        fallback.nested_resume_depth_ = execution_.bitmap_region->entry_depth;
        out += fallback.emit_search_tail(plan, config, dep);
        return out + "} // array fallback\n";
    }
    return out + emit_search_tail(plan, config, dep);
}

std::string CppCodegen::emit_search_tail(const PlanIR &plan, const CodeGenConfig &config, int dep) {
    std::string out;
    if (bitmap_enabled_) {
        out += emit_bitmap_bindings(dep);
        const auto &region = *execution_.bitmap_region;
        const int entry = region.full_region ? region.entry_depth : region.conversion_depth;
        if (dep == entry) return out + emit_bitmap_iter(plan, dep);
    }
    if (config.parType != ParallelType::OpenMP && !execution_.bitmap_region && dep >= nested_resume_depth_)
        out += emit_tbb_call(plan, config, dep + 1);
    out += emit_iter(plan, dep);
    out += emit_search_body(plan, config, dep + 1);
    return out + "}\n";
}
} // namespace minigraph
