#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>

using namespace minigraph;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    size_t selected = 0, full = 0;
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
                    if (ir.bitmap_region->full_region) {
                        ++full;
                        require(code.find("count_local<bitmap_words>") != std::string::npos &&
                                code.find("materialize_local<bitmap_words>") != std::string::npos &&
                                code.find("std::integral_constant<size_t, 0>") != std::string::npos &&
                                code.find("std::integral_constant<size_t, 1>") != std::string::npos &&
                                code.find("std::integral_constant<size_t, 2>") != std::string::npos,
                                "Missing fixed-word region dispatch");
                        const auto start = code.find("// full bitmap region");
                        const auto body = code.substr(start, code.find("} else {", start)-start);
                        require(body.find("graph->N") == std::string::npos &&
                                body.find("bind_input") == std::string::npos,
                                "Array adjacency or conversion inside full bitmap region");
                    }
                    require(code.find("bitmap_region ?") == std::string::npos,
                            "Mixed bitmap/array hot loop");
                    require(code.find(ir.bitmap_region->full_region ? "// full bitmap region" : "->counting_view(") != std::string::npos &&
                            code.find("} // array fallback") != std::string::npos,
                            "Missing prepared count or fallback scope");
                    const auto &slots = ir.bitmap_region->full_region ? ir.bitmap_region->full_sets : ir.bitmap_region->live_ins;
                    const auto &inputs = ir.bitmap_region->full_region ? ir.bitmap_region->full_live_ins : ir.bitmap_region->live_ins;
                    for (int id : inputs) {
                        const auto index = std::find(slots.begin(), slots.end(), id) - slots.begin();
                        const auto binding = "->bind_input(" + std::to_string(index) + ", s" +
                            std::to_string(id) + ");";
                        const auto first = code.find(binding);
                        require(first != std::string::npos && code.find(binding, first+1) == std::string::npos,
                                "Missing or duplicate generated binding");
                    }
                    require(ir.bitmap_region->build_depth == ir.bitmap_region->anchor_depth &&
                            ir.bitmap_region->build_depth <= ir.bitmap_region->entry_depth,
                            "Rows not hoisted to their dependency scope");
                    for (int mutation = 0; mutation < 10; ++mutation) {
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
                auto nested = plan;
                nested.context.config.parType = ParallelType::NestedRt;
                const auto nested_ir = lower_execution(nested);
                if (ir.bitmap_region && ir.bitmap_region->full_region) {
                    require(nested_ir.bitmap_region.has_value(), "Missing TBB full region");
                    const auto code = gen_code(query, nested.context.config, meta);
                    require(code.find("bitmap_for_each") != std::string::npos &&
                            code.find("} // array fallback") != std::string::npos,
                            "Missing task-local bitmap execution");
                }
            }
        }
    }
    require(selected > 0, "No bitmap plans exercised");
    require(full > 0, "No full bitmap regions exercised");
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
            const auto build = code.find("BitmapCountRegion::build_rows");
            const auto enter = code.find("BitmapCountRegion::from_rows");
            const auto root_body = parallel == ParallelType::OpenMP ? code.find("// loop-0 begin")
                                                                  : code.find("// loop-0begin");
            const auto next_loop = code.find("for (size_t i" + std::to_string(anchor + 1) + "_idx", root_body);
            require(build != std::string::npos && next_loop != std::string::npos &&
                    build < next_loop && next_loop < enter,
                    "Generated construction was not moved outside descendant loops");
            require(code.find("BitmapCountRegion::build_rows", build + 1) == std::string::npos,
                    "Duplicate immutable row construction site");
        }
    }
    std::cout << "Validated 48 bitmap planning cases; selected " << selected << " regions, " << full << " full\n";
}
