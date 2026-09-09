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
size_t require_bitmap_temporaries(const std::string &code, const ExecutionIR &ir) {
    const auto &region = *ir.bitmap_region;
    if (region.build_depth == region.anchor_depth && ir.loops.at(region.anchor_depth).read_adjacency)
        require(code.find("auto bitmap_neighbors =") == std::string::npos,
                "Duplicated an available full adjacency view");
    if (!region.full_region) return 0;
    size_t omitted = 0;
    for (const auto &[id, op] : ir.sets) {
        if (op.depth <= region.entry_depth || op.result == SetResult::Count) continue;
        const bool needs_count = op.guard_empty || op.result == SetResult::MaterializeThenCount;
        const auto declaration = "const auto bn" + std::to_string(id) + " =";
        require((code.find(declaration) != std::string::npos) == needs_count,
                "Bitmap materialization count must be declared exactly when consumed");
        if (!needs_count) ++omitted;
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
                    require_bitmap_names(code);
                    omitted_counts += require_bitmap_temporaries(code, nested_ir);
                    require(code.find("bitmap_for_each") != std::string::npos &&
                            code.find("} // array fallback") != std::string::npos,
                            "Missing task-local bitmap execution");
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
        require(code.find("bind_projected_partition") != std::string::npos &&
                code.find("->bind_input(") == std::string::npos, "Direct variant still converts live-in arrays");
        for (int mutation = 0; mutation < 6; ++mutation) {
            auto bad = ir;
            auto &p = bad.bitmap_region->projection_pair;
            if (mutation == 0) ++p->positive;
            if (mutation == 1) ++p->negative;
            if (mutation == 2) ++p->local_depth;
            if (mutation == 3) ++p->external_depth;
            if (mutation == 4) p->fallback_sets.clear();
            if (mutation == 5) p.reset();
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
            const auto build = code.find("BitmapCountRegion::build_rows");
            const auto enter = code.find("BitmapCountRegion::from_rows");
            const auto root_body = parallel == ParallelType::OpenMP ? code.find("// loop-0 begin")
                                                                  : code.find("// loop-0begin");
            const auto next_loop = code.find("for (size_t v" + std::to_string(anchor + 1) + "_idx", root_body);
            require(build != std::string::npos && next_loop != std::string::npos &&
                    build < next_loop && next_loop < enter,
                    "Generated construction was not moved outside descendant loops");
            require(code.find("BitmapCountRegion::build_rows", build + 1) == std::string::npos,
                    "Duplicate immutable row construction site");
        }
    }
    std::cout << "Validated 48 bitmap planning cases; selected " << selected << " regions, " << full << " full\n";
}
