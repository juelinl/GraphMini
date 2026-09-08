#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace minigraph;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Mutate> void rejects(const ExecutionIR &valid, const PlanIR &plan, Mutate mutate) {
    auto invalid = valid;
    mutate(invalid);
    bool rejected = false;
    try {
        verify_execution(invalid, plan);
    } catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected, "Verifier accepted malformed execution IR");
}
} // namespace

int main(int argc, char **argv) {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    const std::vector<std::string> patterns{
        "010101010", "0111100010001000", "0111101011001000",
        "011111100000100000100000100000100000",             // six-vertex star
        "0111111100000010000001000000100000010000001000000" // seven-vertex star
    };
    std::set<SetOpcode> opcodes;
    bool mini = false, iep = false, count = false;
    size_t cases = 0;
    if (argc > 1)
        std::filesystem::create_directories(argv[1]);
    for (const auto &pattern : patterns)
        for (auto mode : {VertexInduced, EdgeInduced, EdgeInducedIEP})
            for (auto pruning : {PruningType::None, PruningType::Static, PruningType::Eager,
                                 PruningType::Online, PruningType::CostModel}) {
                CodeGenConfig config;
                config.adjMatType = mode;
                config.pruningType = pruning;
                config.schedulerType = SchedulerType::GraphPi;
                config.parType = ParallelType::NestedRt;
                auto plan = mode == VertexInduced ? compile_vertex_induced(pattern, config, meta)
                            : mode == EdgeInduced ? compile_edge_induced(pattern, config, meta)
                                                  : compile_edge_induced_iep(pattern, config, meta);
                if (pruning != PruningType::None)
                    plan = create_plan_mg(plan, config);
                const auto execution = lower_execution(plan);
                require(dump_execution(execution) == dump_execution(lower_execution(plan)),
                        "Nondeterministic lowering");
                for (const auto &[id, op] : execution.sets) {
                    for (const auto &step : op.steps)
                        opcodes.insert(step.opcode);
                    count |= op.result == SetResult::Count;
                    require(!op.rules.empty(), "Missing lowering explanation");
                }
                mini |= !execution.minigraphs.empty();
                iep |= !execution.iep.empty();
                rejects(execution, plan,
                        [](auto &ir) { ir.sets.begin()->second.input = {SetSource::Prefix, -42}; });
                rejects(execution, plan, [](auto &ir) {
                    ir.sets.begin()->second.steps.push_back({SetOpcode::Intersect, {}, {}, {}});
                });
                rejects(execution, plan, [](auto &ir) {
                    ir.sets.begin()->second.steps.push_back(
                        {SetOpcode::Bound, {}, VertexReference{999, {}}, {}});
                });
                rejects(execution, plan, [](auto &ir) {
                    ir.sets.begin()->second.result = SetResult::Count;
                    ir.sets.begin()->second.guard_empty = true;
                });
                rejects(execution, plan, [](auto &ir) {
                    ir.loops.front().captured_sets.push_back(ir.sets.begin()->first);
                });
                if (!execution.minigraphs.empty())
                    rejects(execution, plan, [](auto &ir) {
                        auto &mg = ir.minigraphs.begin()->second;
                        mg.parent = mg.id;
                    });
                if (!execution.iep.empty())
                    rejects(execution, plan, [](auto &ir) { ir.iep.front().factors.push_back({}); });
                if (argc > 1) {
                    std::ofstream out(std::filesystem::path(argv[1]) / (std::to_string(cases) + ".ir"));
                    out << dump_execution(execution);
                }
                // Sparse/empty metadata cannot cause integer division by zero in task
                // policy.
                plan.meta.num_edge = 0;
                plan.meta.num_vertex = 0;
                ExecutionIR loops_only;
                lower_loops(plan, loops_only);
                for (const auto &loop : loops_only.loops)
                    require(loop.average_degree == 0 && !loop.cap_threshold,
                            "Invalid empty-graph task policy");
                ++cases;
            }
    require(opcodes.size() == 4 && mini && iep && count, "Missing execution IR coverage");
    std::cout << "Validated " << cases << " execution plans and malformed-IR rejection\n";
}
