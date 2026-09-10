#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <regex>

using namespace minigraph;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void require_bitmap_names(const std::string &code) {
    static const std::regex bit_index(R"(\bv[0-9]+_bit_idx\b)");
    static const std::regex old_names(R"(\b_?i[0-9]+_(id|idx|adj)\b|\bbp[0-9]+\b|\bbitmap_position\b)");
    require(code.find("// Naming (D is matching depth; N is an IR set ID):") == 0,
            "Missing bitmap naming guide");
    require(std::regex_search(code, bit_index), "Missing depth-specific bitmap index");
    require(!std::regex_search(code, old_names), "Legacy bitmap vertex name");
}
void require_bitmap_task_boundaries(const std::string &code, const PlanIR &plan, const ExecutionIR &ir) {
    const auto &region = *ir.bitmap_region;
    const auto compact = std::regex_replace(code, std::regex(R"(\s+)"), "");
    require(compact.find("auto*bitmap_region=&task_state") == std::string::npos,
            "Redundant task state pointer alias");
    size_t calls = 0, offset = 0;
    while ((offset = compact.find("bitmap_for_each(", offset)) != std::string::npos) {
        ++calls;
        ++offset;
    }
    require(calls == static_cast<size_t>(plan.logical.p_size - 2 - region.entry_depth),
            "Changed the number of bitmap task boundaries");
    for (int depth = region.entry_depth + 1; depth <= plan.logical.p_size - 2; ++depth) {
        const auto &loop = ir.loops.at(depth);
        const int iter = plan.logical.iter_set.at(depth - 1).id;
        std::string parallel = loop.spawn_nested ? "true" : "false";
        if (loop.spawn_nested && loop.runtime_threshold) {
            int threshold = loop.threshold_factor * loop.average_degree;
            if (loop.cap_threshold) threshold = std::min(threshold, 100);
            parallel = "input_s" + std::to_string(iter) + ".count()>" + std::to_string(threshold);
        }
        const auto call = "bitmap_for_each(input_s" + std::to_string(iter) + "," + parallel +
            ",[&](size_tbegin,size_tend,boolparallel_task)->uint64_t{";
        require(compact.find(call) != std::string::npos, "Changed bitmap task input, threshold or captures");
        const auto policy = "},bitmap_task_policy," + std::to_string(depth - region.entry_depth - 1) + ");";
        require(compact.find(policy) != std::string::npos, "Changed bitmap task policy or level");
        const auto body = "autobitmap_level" + std::to_string(depth) + "=[&](constBitmap&input_s";
        require(compact.find(body) != std::string::npos, "Missing synchronous level wrapper");
        for (int id : region.loop_inputs.at(depth)) {
            require(ir.sets.at(id).depth < depth, "Captured a private output as input");
            require(compact.find("BitmapTaskInputtask_s" + std::to_string(id)) != std::string::npos,
                    "Missing explicit task input lifetime");
        }
    }
}
size_t require_bitmap_temporaries(const std::string &code, const ExecutionIR &ir) {
    const auto &region = *ir.bitmap_region;
    if (region.build_depth == region.anchor_depth && ir.loops.at(region.anchor_depth).read_adjacency)
        require(code.find("auto bitmap_neighbors =") == std::string::npos,
                "Duplicated an available full adjacency view");
    if (!region.full_region) return 0;
    size_t omitted = 0;
    for (const auto &[id, op] : ir.sets) {
        if (op.depth <= region.entry_depth || op.result == SetResult::Count) continue;
        require(code.find("const auto bn" + std::to_string(id) + " =") == std::string::npos,
                "Redundant bitmap count temporary");
        require(code.find("Bitmap s" + std::to_string(id) + "(bitgraph.universe())") != std::string::npos,
                "Missing named private bitmap output");
        ++omitted;
    }
    return omitted;
}
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    size_t selected = 0, full = 0, omitted_counts = 0;
    for (int n = 4; n <= 7; ++n) {
        for (int missing = 0; missing < 4; ++missing) {
            std::string query(n * n, '1');
            for (int i = 0; i < n; ++i)
                query[i * n + i] = '0';
            if (missing)
                query[1] = query[n] = '0';
            if (missing == 2)
                query[2] = query[2 * n] = '0';
            if (missing == 3)
                query[2 * n + 3] = query[3 * n + 2] = '0';
            for (auto scheduler :
                 {SchedulerType::GraphPi, SchedulerType::GraphMini, SchedulerType::GraphZero}) {
                CodeGenConfig config;
                config.pruningType = PruningType::None;
                config.parType = ParallelType::OpenMP;
                config.schedulerType = scheduler;
                config.bitmap = true;
                const auto plan = compile_vertex_induced(query, config, meta);
                const auto ir = lower_execution(plan);
                require(dump_execution(ir) == dump_execution(lower_execution(plan)),
                        "Nondeterministic bitmap plan");
                if (ir.bitmap_region) {
                    ++selected;
                    const auto code = gen_code(query, config, meta);
                    require_bitmap_names(code);
                    omitted_counts += require_bitmap_temporaries(code, ir);
                    if (ir.bitmap_region->full_region) {
                        ++full;
                        const auto compact = std::regex_replace(code, std::regex(R"(\s+)"), "");
                        require(code.find("_count<bitmap_words>") != std::string::npos &&
                                code.find(".assign_") != std::string::npos &&
                                compact.find("dispatch_bitmap_words(bitgraph.universe().size(),bitmap_execute)") != std::string::npos &&
                                code.find("#include \"backend/bitmap_dispatch.h\"") != std::string::npos,
                                "Missing fixed-word region dispatch");
                        const auto start = code.find("// full bitmap region");
                        const auto body = code.substr(start, code.find("} else {", start)-start);
                        require(body.find("graph->N") == std::string::npos &&
                                body.find("bind_input") == std::string::npos,
                                "Array adjacency or conversion inside full bitmap region");
                    }
                    require(code.find("bitmap_region ?") == std::string::npos,
                            "Mixed bitmap/array hot loop");
                    require(code.find("materialize_local<") == std::string::npos &&
                            code.find("count_local<") == std::string::npos &&
                            code.find("task_state") == std::string::npos,
                            "Generated bitmap execution still uses numbered slots");
                    require(code.find(ir.bitmap_region->full_region ? "// full bitmap region" : "// bitmap terminal region") != std::string::npos &&
                            code.find("} // array fallback") != std::string::npos,
                            "Missing prepared count or fallback scope");
                    const auto &inputs = ir.bitmap_region->full_region ? ir.bitmap_region->full_live_ins : ir.bitmap_region->live_ins;
                    for (int id : inputs) {
                        const auto binding = "bitmap_s" + std::to_string(id) + ".emplace(Bitmap::from_sorted(";
                        const auto first = code.find(binding);
                        require(first != std::string::npos && code.find(binding, first+1) == std::string::npos,
                                "Missing or duplicate generated binding");
                    }
                    require(ir.bitmap_region->build_depth == ir.bitmap_region->anchor_depth &&
                            ir.bitmap_region->build_depth <= ir.bitmap_region->entry_depth,
                            "Rows not hoisted to their dependency scope");
                    require(dump_execution(ir).find("bitmap-bind set") != std::string::npos,
                            "Missing binding metadata in IR dump");
                    for (int mutation = 0; mutation < 21; ++mutation) {
                        auto bad = ir;
                        auto &r = *bad.bitmap_region;
                        if (mutation == 0)
                            ++r.entry_depth;
                        if (mutation == 1)
                            r.anchor_depth = n;
                        if (mutation == 2)
                            r.conversion_depth = -1;
                        if (mutation == 3)
                            r.live_ins.clear();
                        if (mutation == 4)
                            r.count_ops.clear();
                        if (mutation == 5)
                            r.iterator_set = -1;
                        if (mutation == 6) r.full_region = !r.full_region;
                        if (mutation == 7) r.full_sets.push_back(-1);
                        if (mutation == 8) r.full_live_ins.push_back(-1);
                        if (mutation == 9) ++r.build_depth;
                        if (mutation == 10) r.slots.clear();
                        if (mutation == 11) ++r.slots.begin()->second;
                        if (mutation == 12) r.slots.emplace(-1, 0);
                        if (mutation == 13) r.bindings.clear();
                        if (mutation == 14) r.bindings.push_back(r.bindings.front());
                        if (mutation == 15) r.bindings.front().set_id = -1;
                        if (mutation == 16) r.bindings.front().slot = -1;
                        if (mutation == 17) --r.bindings.front().depth;
                        if (mutation == 18) ++r.bindings.front().depth;
                        if (mutation == 19) r.loop_inputs[-1] = {0};
                        if (mutation == 20) r.loop_outputs[-1] = {0};
                        bool rejected = false;
                        try {
                            verify_execution(bad, plan);
                        } catch (const std::logic_error &) {
                            rejected = true;
                        }
                        require(rejected, "Accepted malformed bitmap region");
                    }
                } else
                    require(!ir.bitmap_reason.empty(), "Missing fallback explanation");
                auto arrays = plan;
                arrays.context.config.bitmap = false;
                require(!lower_execution(arrays).bitmap_region, "Default selected bitmap");
                auto unsupported = plan;
                unsupported.context.config.runnerType = RunnerType::Profiling;
                require(!lower_execution(unsupported).bitmap_region,
                        "Unsupported profiling accepted");
                for (auto parallel : {ParallelType::TbbTop, ParallelType::Nested, ParallelType::NestedRt}) {
                    auto nested = plan;
                    nested.context.config.parType = parallel;
                    const auto nested_ir = lower_execution(nested);
                    if (ir.bitmap_region && ir.bitmap_region->full_region) {
                        require(nested_ir.bitmap_region.has_value(), "Missing TBB full region");
                        const auto code = gen_code(query, nested.context.config, meta);
                        require_bitmap_names(code);
                        omitted_counts += require_bitmap_temporaries(code, nested_ir);
                        require_bitmap_task_boundaries(code, nested, nested_ir);
                        require(code.find("bitmap_for_each") != std::string::npos &&
                                code.find("} // array fallback") != std::string::npos,
                                "Missing task-local bitmap execution");
                    }
                }
            }
        }
    }
    require(selected > 0, "No bitmap plans exercised");
    require(full > 0, "No full bitmap regions exercised");
    require(omitted_counts > 0, "Unused bitmap count elimination was not exercised");
    {
        CodeGenConfig config;
        config.pruningType = PruningType::None;
        config.schedulerType = SchedulerType::Outgoing;
        config.bitmap = config.bitmapDirect = true;
        const std::string query = "011011101101110110011011101101110110";
        auto plan = compile_vertex_induced(query, config, meta);
        auto ir = lower_execution(plan);
        if (ir.bitmap_region && !ir.bitmap_region->projection_pair) std::cerr << dump_execution(ir);
        require(ir.bitmap_region && ir.bitmap_region->projection_pair, "Missing algebraic projected partition");
        const auto code = gen_code(query, config, meta);
        require(code.find("// shared bounded neighborhood projection") != std::string::npos &&
                code.find("->bind_input(") == std::string::npos, "Direct variant still converts live-in arrays");
        for (int mutation = 0; mutation < 9; ++mutation) {
            auto bad = ir;
            auto &p = bad.bitmap_region->projection_pair;
            if (mutation == 0) ++p->positive;
            if (mutation == 1) ++p->negative;
            if (mutation == 2) ++p->local_depth;
            if (mutation == 3) ++p->external_depth;
            if (mutation == 4) p->fallback_sets.clear();
            if (mutation == 5) p.reset();
            if (mutation == 6) bad.bitmap_region->slots.erase(p->positive);
            if (mutation == 7) ++bad.bitmap_region->bindings.front().depth;
            if (mutation == 8) bad.bitmap_region->bindings.front().slot = -1;
            bool rejected = false;
            try { verify_execution(bad, plan); } catch (const std::logic_error &) { rejected = true; }
            require(rejected, "Accepted invalid projected partition");
        }
        plan.context.config.bitmapDirect = false;
        require(!lower_execution(plan).bitmap_region->projection_pair, "Enabled direct lowering by default");
        // Selection is structural, not dependent on the original vertex labels.
        std::vector<int> order{0, 1, 2, 3, 4, 5};
        for (int permutation = 0; permutation < 12; ++permutation) {
            std::string renamed;
            for (int i : order) for (int j : order) renamed += query[i*6+j];
            auto renamed_ir = lower_execution(compile_vertex_induced(renamed, config, meta));
            require(renamed_ir.bitmap_region && renamed_ir.bitmap_region->projection_pair,
                    "Projected partition depended on pattern labels");
            std::next_permutation(order.begin(), order.end());
        }
    }
    // No universal query root: execution starts at depth 2, but immutable rows
    // depend only on anchor 0 (octahedron) or anchor 1 (atlas 145).
    for (const auto &[query, anchor] : std::vector<std::pair<std::string, int>>{
             {"011011101101110110011011101101110110", 0},
             {"011011101000110101001010100100101000", 1}}) {
        for (auto parallel : {ParallelType::OpenMP, ParallelType::NestedRt}) {
            CodeGenConfig config;
            config.pruningType = PruningType::None;
            config.schedulerType = SchedulerType::Outgoing;
            config.parType = parallel;
            config.bitmap = true;
            const auto ir = lower_execution(compile_vertex_induced(query, config, meta));
            require(ir.bitmap_region && ir.bitmap_region->full_region, "Missing late-entry test region");
            require(ir.bitmap_region->build_depth == anchor && ir.bitmap_region->entry_depth == 2,
                    "Conflated row construction with execution scope");
            const auto code = gen_code(query, config, meta);
            const auto build = code.find("BitGraph::build");
            const auto enter = code.find("std::optional<Bitmap> bitmap_s");
            const auto root_body = parallel == ParallelType::OpenMP ? code.find("// loop-0 begin")
                                                                  : code.find("// loop-0begin");
            const auto next_loop = code.find("for (size_t v" + std::to_string(anchor + 1) + "_idx", root_body);
            require(build != std::string::npos && next_loop != std::string::npos &&
                    build < next_loop && next_loop < enter,
                    "Generated construction was not moved outside descendant loops");
            require(code.find("BitGraph::build", build + 1) == std::string::npos,
                    "Duplicate immutable row construction site");
        }
    }
    std::cout << "Validated 48 bitmap planning cases; selected " << selected << " regions, " << full << " full\n";
}
