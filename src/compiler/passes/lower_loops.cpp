#include "compiler/execution_ir.h"
#include <algorithm>

namespace minigraph {
namespace {
void capture(std::vector<int> &ids, int id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end())
        ids.push_back(id);
}
} // namespace
void lower_loops(const PlanIR &plan, ExecutionIR &execution) {
    execution.loops.resize(plan.set_ops.size());
    for (size_t loop = 0; loop < execution.loops.size(); ++loop) {
        auto &out = execution.loops[loop];
        out.read_adjacency = plan.config.pruningType == PruningType::None;
        if (!out.read_adjacency)
            for (const auto &op : plan.set_ops.at(loop))
                if (!plan.get_parent_mg(op))
                    out.read_adjacency = true;
        out.spawn_nested = loop > 0 && static_cast<int>(loop) < plan.get_serial_loop() &&
                           (plan.config.parType == ParallelType::Nested ||
                            plan.config.parType == ParallelType::NestedRt);
        out.runtime_threshold = plan.config.parType == ParallelType::NestedRt;
        // Keep the existing degree heuristic; empty graphs must not divide by zero.
        out.average_degree = plan.meta.num_vertex ? plan.meta.num_edge / plan.meta.num_vertex : 0;
        out.cap_threshold = out.average_degree > 0 && plan.meta.max_degree / out.average_degree > 100;
        if (loop == 0)
            continue;
        if (plan.config.pruningType != PruningType::None) {
            for (int dep = loop; dep < plan.p_size - 1; ++dep)
                for (const auto &mg : plan.mg_used.at(dep))
                    if (mg.loop_depth() < static_cast<int>(loop))
                        capture(out.captured_minigraphs, mg.id);
            for (const auto &mg : plan.mg_ops.at(loop)) {
                const auto parent = plan.get_parent_mg(mg);
                if (parent && parent->loop_depth() < static_cast<int>(loop))
                    capture(out.captured_minigraphs, parent->id);
            }
        }
        for (const auto &op : plan.set_ops.at(loop)) {
            const auto parent = plan.get_parent_vset(op, loop - 1);
            if (parent && parent->id != plan.iter_set.at(loop - 1).id)
                capture(out.captured_sets, parent->id);
        }
        for (int depth = loop; depth < plan.p_size - 1; ++depth)
            for (const auto &op : plan.set_ops.at(depth))
                if (!plan.get_parent_vset(op, loop - 1))
                    for (size_t prior = 0; prior < loop; ++prior)
                        out.captured_adjacencies.insert(prior);
    }
}
} // namespace minigraph
