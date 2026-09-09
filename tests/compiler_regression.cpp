#include "compiler/codegen.h"
#ifdef GRAPHMINI_REFACTORED
#include "compiler/planning.h"
#include "compiler/scheduling/iep_redundancy.hpp"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>

using namespace minigraph;

int main(int argc, char **argv) {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    const std::vector<std::string> patterns{
        "011101110",        // triangle
        "010101010",        // path with three vertices
        "0111100010001000", // star with three leaves (IEP eligible)
        "0111101111011110", // four-clique
        "0111101011001000"  // triangle with a pendant vertex
    };
    size_t cases = 0;
    if (argc > 1)
        std::filesystem::create_directories(argv[1]);
    for (const auto &pattern : patterns)
        for (auto mode : {VertexInduced, EdgeInduced, EdgeInducedIEP})
            for (auto scheduler :
                 {SchedulerType::GraphPi, SchedulerType::GraphMini, SchedulerType::GraphZero})
                for (auto pruning : {PruningType::None, PruningType::Static, PruningType::Eager,
                                     PruningType::Online, PruningType::CostModel})
                    for (auto parallel : {ParallelType::OpenMP, ParallelType::TbbTop, ParallelType::Nested,
                                          ParallelType::NestedRt})
                        for (auto runner : {RunnerType::Benchmark, RunnerType::Profiling}) {
                            CodeGenConfig config;
                            config.adjMatType = mode;
                            config.schedulerType = scheduler;
                            config.pruningType = pruning;
                            config.parType = parallel;
                            config.runnerType = runner;
                            const auto code = gen_code(pattern, config, meta);
                            if (code.empty())
                                throw std::runtime_error("Empty generated code");
#ifdef GRAPHMINI_REFACTORED
                            if (code.find("// Naming (D is matching depth; N is an IR set ID):") != 0 ||
                                code.find("v0_adj") == std::string::npos ||
                                code.find("v1_idx") == std::string::npos)
                                throw std::runtime_error("Missing generated naming guide or vertex names");
                            static const std::regex old_names(R"(\b_?i[0-9]+_(id|idx|adj)\b|\bbp[0-9]+\b)");
                            if (std::regex_search(code, old_names))
                                throw std::runtime_error("Legacy generated vertex name");
                            if (code.find("/* VSet(") != std::string::npos ||
                                code.find("// operator begin") != std::string::npos ||
                                code.find("// operator end") != std::string::npos)
                                throw std::runtime_error("Redundant generated comments");
                            if (parallel != ParallelType::OpenMP) {
                                if (code.find("struct QueryContext") == std::string::npos ||
                                    code.find("Loop0(query)") == std::string::npos ||
                                    code.find("query.ctx") == std::string::npos ||
                                    code.find("query.graph") == std::string::npos ||
                                    code.find("static const Graph") != std::string::npos ||
                                    code.find("static BenchmarkProgress") != std::string::npos)
                                    throw std::runtime_error("Missing explicit per-query task context");
                                static const std::regex old_context_call(R"(Loop[0-9]+\(ctx\b)");
                                if (std::regex_search(code, old_context_call))
                                    throw std::runtime_error("Nested task did not receive query context");
                                const bool progress = runner != RunnerType::Profiling;
                                if ((code.find("query.progress.root(") != std::string::npos) != progress)
                                    throw std::runtime_error("Incorrect query-context progress wiring");
                            }
#endif
                            if (argc > 1) {
                                std::ofstream out(std::filesystem::path(argv[1]) /
                                                  (std::to_string(cases) + ".cpp"));
                                out << code;
                            }
                            ++cases;
                        }
#ifdef GRAPHMINI_REFACTORED
    // A pruned clique needs v1 for a bound, but the terminal MiniGraph
    // operation consumes only its iterator index and adjacency owner.
    for (auto parallel : {ParallelType::OpenMP, ParallelType::TbbTop,
                          ParallelType::Nested, ParallelType::NestedRt}) {
        CodeGenConfig cleanup;
        cleanup.schedulerType = SchedulerType::GraphPi;
        cleanup.pruningType = PruningType::Static;
        cleanup.parType = parallel;
        const auto code = gen_code(patterns[3], cleanup, meta);
        if (code.find("const IdType v1 =") == std::string::npos ||
            code.find(".bounded(v1)") == std::string::npos ||
            code.find("const IdType v2 =") != std::string::npos ||
            code.find("using MiniGraphType =") != std::string::npos)
            throw std::runtime_error("Incorrect MiniGraph declaration liveness");
    }
    // Sorting IR must be asymmetric even when edge/restriction counts disagree.
    VertexSetIR sparse(EdgeIR(1), EdgeRestrictIR(3), 2, EdgeInduced);
    VertexSetIR dense(EdgeIR(3), EdgeRestrictIR(0), 2, EdgeInduced);
    if (!(sparse < dense) || dense < sparse || sparse < sparse)
        throw std::runtime_error("IR ordering is not a strict weak order");
    // Planning another query mode must not change an existing IR's semantics.
    CodeGenConfig config;
    config.schedulerType = SchedulerType::GraphPi;
    auto vertex = compile_vertex_induced(patterns[1], config, meta);
    const auto before = create_plan_mg(vertex, vertex.context.config);
    auto edge = compile_edge_induced(patterns[1], config, meta);
    for (const auto &ops : vertex.logical.set_ops)
        for (const auto &op : ops)
            if (op.adjMatType != VertexInduced || op.same_iep_computation(op))
                throw std::runtime_error("Query mode leaked into existing vertex IR");
    const auto after = create_plan_mg(vertex, vertex.context.config);
    if (before.auxiliary.mg_ops != after.auxiliary.mg_ops)
        throw std::runtime_error("Auxiliary planning is not isolated");
    auto iep = compile_edge_induced_iep(patterns[2], config, meta);
    if (iep.counting.iep_num <= 1 || iep.counting.iep_set.empty())
        throw std::runtime_error("IEP path was not exercised");
    for (auto scheduler : {SchedulerType::GraphPi, SchedulerType::GraphMini,
                           SchedulerType::GraphZero}) {
        config.schedulerType = scheduler;
        if (compile_edge_induced_iep(patterns[2], config, meta).counting.iep_redundancy != 2)
            throw std::runtime_error("Incorrect star IEP symmetry factor");
    }
    if (iep_redundancy(5, 3, {{1, 2}, {2, 3}, {3, 4}}) != 6 ||
        iep_redundancy(5, 3, {{1, 2}, {1, 3}, {1, 4}}) != 1 ||
        iep_redundancy(5, 3, {{1, 2}, {2, 3}, {1, 4}}) != 2 ||
        iep_redundancy(4, 1, {{1, 2}, {2, 3}}) != 1)
        throw std::runtime_error("Incorrect general IEP symmetry factor");
#endif
    std::cout << "Validated " << cases << " compiler configurations\n";
}
