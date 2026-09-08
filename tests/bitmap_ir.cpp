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
                    for (int mutation = 0; mutation < 9; ++mutation) {
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
                unsupported.context.config.parType = ParallelType::NestedRt;
                require(!lower_execution(unsupported).bitmap_region,
                        "Unsupported task capture accepted");
                if (n == 4 && missing == 0 && scheduler == SchedulerType::GraphPi)
                    require(gen_code(query, unsupported.context.config, meta).find("// bitmap: requires") == 0,
                            "Missing generated-code fallback explanation");
            }
        }
    }
    require(selected > 0, "No bitmap plans exercised");
    require(full > 0, "No full bitmap regions exercised");
    std::cout << "Validated 48 bitmap planning cases; selected " << selected << " regions, " << full << " full\n";
}
