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
    const auto begin = code.find("// full bitmap region");
    if (begin == std::string::npos)
        return {};
    const auto end = code.find("} else {", begin);
    if (end == std::string::npos)
        throw std::runtime_error("Missing array fallback");
    auto body = std::regex_replace(code.substr(begin, end - begin), std::regex(R"(\s+)"), "");
    body = std::regex_replace(body, std::regex(R"(bitmap_region->|task_state\.)"), "state.");
    body = std::regex_replace(body, std::regex("returncounter;"), "continue;");
    static const std::regex statement(
        R"(state\.(count_local|materialize_local)<bitmap_words>\([^;]+;|if\(!bn[0-9]+\)continue;|constautobn[0-9]+=|counter\+=bn[0-9]+;|bitmap_counters\[3\]\.fetch_add\([^;]+;)");
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
    CppCodegen writer(config, ir);
    const auto code = writer.emit_omp(plan, config);
    const auto build = code.find("BitmapCountRegion::build_rows");
    const auto state = code.find("BitmapCountRegion::from_rows");
    const auto count = code.find("->counting_view(");
    if (count == std::string::npos || !(build < state && state < count) ||
        code.find("// full bitmap region") != std::string::npos ||
        code.find("} // array fallback") == std::string::npos ||
        code.find("bitmap_counters[3].fetch_add") == std::string::npos)
        throw std::runtime_error("Broken count-only region scaffolding");
    for (size_t i = 0; i < region.live_ins.size(); ++i) {
        const auto binding = "->bind_input(" + std::to_string(i) + ", s" + std::to_string(region.live_ins[i]) + ");";
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
