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

int main(int argc, char **argv) {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    if (argc > 1)
        std::filesystem::create_directories(argv[1]);
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
