#include "compiler/codegen.h"
#include "compiler/planning.h"
#include "compiler/compilation_profile.h"
#include "runtime/nested_policy.h"
#include <limits>
#include <stdexcept>

using namespace minigraph;
int main() {
    if (nested_threshold(0, 0, 0, 4) != 0 ||
        nested_threshold(100, 2800, 500, 4) != 112 ||
        nested_threshold(100, 2800, 3000, 4) != 100 ||
        nested_threshold(1, std::numeric_limits<uint64_t>::max(), 0, 4) !=
            std::numeric_limits<size_t>::max())
        throw std::runtime_error("Runtime degree heuristic changed");
    CodeGenConfig config;
    config.schedulerType = SchedulerType::Outgoing;
    config.pruningType = PruningType::None;
    config.parType = ParallelType::NestedRt;
    config.bitmap = config.bitmapDirect = true;
    std::string clique(25, '1');
    for (int v = 0; v < 5; ++v) clique[v * 5 + v] = '0';
    for (auto semantics : {AdjMatType::EdgeInduced, AdjMatType::VertexInduced}) {
        config.adjMatType = semantics;
        const auto empty = gen_code(clique, config, MetaData{});
        const auto schedule = schedule_query(clique, config, MetaData{});
        CompilationProfile reused;
        std::string from_schedule;
        {
            CompilationCapture capture(reused);
            from_schedule = gen_code(clique, config, MetaData{}, schedule);
        }
        if (from_schedule != empty || reused.seconds.count("scheduling"))
            throw std::runtime_error("Existing schedule was regenerated or changed emitted code");
        const auto dense = gen_code(clique, config, MetaData(100, 2800, 900, 500, 50, 200));
        const auto skewed = gen_code(clique, config, MetaData(100, 2800, 900, 3000, 50, 200));
        if (empty != dense || empty != skewed || empty.find("query.nested_thresholds[") == std::string::npos)
            throw std::runtime_error("Offline code still depends on graph statistics");
    }
}
