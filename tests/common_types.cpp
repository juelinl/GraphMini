#include "common/types.h"
#include "common/counter.h"
#include "compiler/config.h"

#include <type_traits>

static_assert(std::is_same_v<minigraph::IdType, std::uint32_t>);
static_assert(sizeof(minigraph::IdType) == 4);
static_assert(minigraph::INVALID_ID == UINT32_MAX);

int main() {
    minigraph::Counter counter;
    counter.push_back(1);
    counter.push_back("ignored payload");
    const minigraph::CodeGenConfig config;
    return counter.count != 2 || config.bitmap || config.bitmapDiagnostics ||
           config.adjMatType != minigraph::AdjMatType::VertexInduced ||
           config.schedulerType != minigraph::SchedulerType::GraphMini ||
           config.pruningType != minigraph::PruningType::Eager ||
           config.parType != minigraph::ParallelType::NestedRt ||
           config.runnerType != minigraph::RunnerType::Benchmark;
}
