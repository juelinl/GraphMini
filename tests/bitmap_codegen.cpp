#include "compiler/codegen.h"
#include "compiler/codegen/cpp.h"
#include "compiler/codegen/format.h"
#include "compiler/codegen/names.h"
#include "compiler/planning.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minigraph;

// Compare operation statements independently of loop/task scaffolding and
// order: OpenMP emits outer-to-inner loops, while TBB declares inner tasks
// first.
std::vector<std::string> bitmap_operations(const std::string &code) {
    const bool classes = code.find("// Bitmap level definitions:") != std::string::npos;
    const auto begin = code.find(classes ? "// Bitmap level definitions:" : "// full bitmap region");
    if (begin == std::string::npos)
        return {};
    const auto end = code.find(classes ? "// End bitmap level definitions." : "} else {", begin);
    if (end == std::string::npos)
        throw std::runtime_error("Missing array fallback");
    auto body = std::regex_replace(code.substr(begin, end - begin), std::regex(R"(\s+)"), "");
    static const std::regex statement(
        R"(s[0-9]+\.(intersection_count|subtraction_count|bounded_count|removed_count|assign_intersection|assign_subtraction|assign_bounded|assign_removed)<bitmap_words>\([^;]+;|if\(!s[0-9]+\.count\(\)\)continue;|counter\+=s[0-9]+\.count\(\);|bitmap_counters\[3\]\.fetch_add\([^;]+;)");
    std::vector<std::string> result;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), statement); it != std::sregex_iterator(); ++it)
        result.push_back(it->str());
    std::sort(result.begin(), result.end());
    return result;
}

std::string check_count_only_emission() {
    CodeGenConfig config;
    config.schedulerType = SchedulerType::Outgoing;
    config.pruningType = PruningType::None;
    config.parType = ParallelType::OpenMP;
    config.bitmap = config.bitmapDiagnostics = true;
    std::string query(36, '1');
    for (int v = 0; v < 6; ++v)
        query[v * 6 + v] = '0';
    const auto plan = compile_vertex_induced(query, config, MetaData(100, 1000, 600, 30, 20, 60));
    auto ir = lower_execution(plan);
    if (!ir.bitmap_region || !ir.bitmap_region->full_region)
        throw std::runtime_error("Missing full-region fixture");
    // Exercise the count-only emitter even when planning prefers the full
    // extension. Its terminal live-ins/counts are retained by the lowering.
    // This is an emitter fixture, not a change to region selection policy.
    auto &region = *ir.bitmap_region;
    region.full_region = false;
    region.full_sets.clear();
    region.full_live_ins.clear();
    region.projection_pair.reset();
    lower_bitmap_bindings(ir, region);
    verify_bitmap_bindings(ir, region);
    bool late_binding = false;
    for (const auto &binding : region.bindings) {
        late_binding |= binding.depth > region.entry_depth;
        if (binding.depth != ir.sets.at(binding.set_id).depth)
            throw std::runtime_error("Count-only fixture lost definition-scoped binding");
    }
    if (!late_binding) throw std::runtime_error("Missing delayed-binding coverage");
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto bad = region;
        if (mutation == 0) bad.bindings.front().depth = bad.entry_depth;
        if (mutation == 1) ++bad.bindings.front().depth;
        if (mutation == 2) bad.bindings.front().slot = -1;
        if (mutation == 3) bad.bindings.clear();
        if (mutation == 4) bad.slots.erase(bad.bindings.front().set_id);
        if (mutation == 5) { bad.live_ins.clear(); bad.slots.clear(); bad.bindings.clear(); }
        bool rejected = false;
        try { verify_bitmap_bindings(ir, bad); }
        catch (const std::logic_error &) { rejected = true; }
        if (!rejected) throw std::runtime_error("Accepted invalid count-only binding lifetime");
    }
    CppCodegen writer(config, ir);
    const auto code = writer.emit_omp(plan, config);
    const auto build = code.find("BitGraph::build");
    const auto state = code.find("std::optional<Bitmap> bitmap_s");
    const auto count = code.find("// bitmap terminal region");
    if (count == std::string::npos || !(build < state && state < count) ||
        code.find("// full bitmap region") != std::string::npos ||
        code.find("} // array fallback") == std::string::npos ||
        code.find("bitmap_counters[3].fetch_add") == std::string::npos)
        throw std::runtime_error("Broken count-only region scaffolding");
    for (size_t i = 0; i < region.live_ins.size(); ++i) {
        const auto binding = "bitmap_s" + std::to_string(region.live_ins[i]) + ".emplace(Bitmap::from_sorted(";
        const auto position = code.find(binding);
        if (!(state < position && position < count) || code.find(binding, position + 1) != std::string::npos)
            throw std::runtime_error("Missing or duplicate count-only live-in binding");
    }
    return code;
}

int main(int argc, char **argv) {
    const auto count_only = check_count_only_emission();
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    if (argc > 1) {
        std::filesystem::create_directories(argv[1]);
        const auto fixtures = std::filesystem::path(argv[1]) / "fixtures";
        std::filesystem::create_directories(fixtures);
        std::ofstream out(fixtures / "count-only.cpp");
        out << count_only;
        if (!out) throw std::runtime_error("Cannot save count-only fixture");
    }
    size_t cases = 0, full = 0;
    std::set<SetOpcode> opcodes;
    for (int n = 4; n <= 8; ++n)
        for (int missing = 0; missing < 4; ++missing) {
            std::string query(n * n, '1');
            for (int v = 0; v < n; ++v)
                query[v * n + v] = '0';
            if (missing)
                query[1] = query[n] = '0';
            if (missing == 2)
                query[2] = query[2 * n] = '0';
            if (missing == 3)
                query[2 * n + 3] = query[3 * n + 2] = '0';
            for (auto mode : {VertexInduced, EdgeInduced}) {
                CodeGenConfig config;
                config.adjMatType = mode;
                config.schedulerType = SchedulerType::Outgoing;
                config.pruningType = PruningType::None;
                config.bitmap = true;
                // Scheduling is independent of these emission options. Reuse
                // one plan so eight-vertex coverage stays practical in CTest.
                auto plan = mode == VertexInduced ? compile_vertex_induced(query, config, meta)
                                                  : compile_edge_induced(query, config, meta);
                for (bool direct : {false, true})
                    for (bool diagnostics : {false, true}) {
                        std::vector<std::string> serial;
                        for (auto parallel : {ParallelType::OpenMP, ParallelType::TbbTop, ParallelType::Nested,
                                              ParallelType::NestedRt}) {
                            config.parType = parallel;
                            config.bitmapDirect = direct;
                            config.bitmapDiagnostics = diagnostics;
                            plan.context.config = config;
                            const auto ir = lower_execution(plan);
                            CppCodegen writer(config, ir);
                            const auto code = format_generated_cpp(
                                std::string(codegen_names::guide) +
                                (parallel == ParallelType::OpenMP
                                     ? writer.emit_omp(plan, config)
                                     : "// bitmap: " + ir.bitmap_reason + "\n" + writer.emit_nested(plan, config)));
                            if (ir.bitmap_region && ir.bitmap_region->full_region)
                                for (const auto &[id, op] : ir.sets)
                                    if (op.depth > ir.bitmap_region->entry_depth)
                                        for (const auto &step : op.steps)
                                            opcodes.insert(step.opcode);
                            const auto operations = bitmap_operations(code);
                            if (!operations.empty())
                                ++full;
                            if (parallel == ParallelType::OpenMP)
                                serial = operations;
                            else if (operations != serial)
                                throw std::runtime_error("OpenMP/TBB bitmap operations differ at case " +
                                                         std::to_string(cases));
                            if (argc > 1) {
                                std::ofstream out(std::filesystem::path(argv[1]) / (std::to_string(cases) + ".cpp"));
                                out << code;
                                if (!out)
                                    throw std::runtime_error("Cannot save generated kernel");
                            }
                            ++cases;
                        }
                    }
            }
        }
    if (!full)
        throw std::runtime_error("Missing full bitmap coverage");
    if (opcodes.size() != 4)
        throw std::runtime_error("Missing bitmap opcode coverage");
    std::cout << "Compared " << cases << " bitmap configurations (" << full << " full regions)\n";
}
