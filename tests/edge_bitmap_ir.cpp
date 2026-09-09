#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <iostream>
#include <stdexcept>
using namespace minigraph;
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    int selected = 0, removals = 0, bounds = 0;
    for (int n : {6, 7, 8})
        for (int missing = 0; missing < 3; ++missing) {
            std::string query(n*n, '1');
            for (int i = 0; i < n; ++i) query[i*n+i] = '0';
            if (missing) query[1*n+2] = query[2*n+1] = '0';
            if (missing == 2) query[2*n+3] = query[3*n+2] = '0';
            CodeGenConfig config;
            config.schedulerType = SchedulerType::Outgoing;
            config.pruningType = PruningType::None;
            config.parType = ParallelType::NestedRt;
            config.bitmap = true;
            const auto plan = compile_edge_induced(query, config, meta);
            const auto ir = lower_execution(plan);
            if (!ir.bitmap_region || !ir.bitmap_region->full_region)
                throw std::runtime_error("Missing edge-induced full region");
            ++selected;
            for (const auto &[id, op] : ir.sets)
                for (const auto &step : op.steps) {
                    if (step.opcode == SetOpcode::DifferenceExcludingOwner)
                        throw std::runtime_error("Edge-induced nonedge used neighborhood subtraction");
                    if (op.depth > ir.bitmap_region->entry_depth) {
                        removals += step.opcode == SetOpcode::Remove;
                        bounds += step.opcode == SetOpcode::Bound;
                    }
                }
            const auto iep = lower_execution(compile_edge_induced_iep(query, config, meta));
            if (iep.bitmap_region || iep.iep_bitmap)
                throw std::runtime_error("IEP selected bitmap");
        }
    if (!removals || !bounds) throw std::runtime_error("Missing unary coverage");
    std::cout << "Edge regions=" << selected << " removals=" << removals << " bounds=" << bounds << '\n';
}
