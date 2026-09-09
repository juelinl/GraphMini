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
    // Scheduling policy belongs to lowering, not to the logical constraints.
    execution.serial_loop_boundary = plan.counting.iep_num <= 1
        ? std::max(1, plan.logical.p_size - 2)
        : std::max(1, plan.logical.p_size - plan.counting.iep_num - 1);
    execution.loops.resize(plan.logical.set_ops.size());
    for (size_t loop = 0; loop < execution.loops.size(); ++loop) {
        auto &out = execution.loops[loop];
        out.read_adjacency = plan.context.config.pruningType == PruningType::None;
        if (!out.read_adjacency)
            for (const auto &op : plan.logical.set_ops.at(loop))
                if (!plan.get_parent_mg(op))
                    out.read_adjacency = true;
        out.spawn_nested = loop > 0 && static_cast<int>(loop) < execution.serial_loop_boundary &&
                           (plan.context.config.parType == ParallelType::Nested ||
                            plan.context.config.parType == ParallelType::NestedRt);
        out.runtime_threshold = plan.context.config.parType == ParallelType::NestedRt;
        // Keep the existing degree heuristic; empty graphs must not divide by zero.
        out.average_degree = plan.context.meta.num_vertex ? plan.context.meta.num_edge / plan.context.meta.num_vertex : 0;
        out.cap_threshold = out.average_degree > 0 && plan.context.meta.max_degree / out.average_degree > 100;
        if (loop == 0)
            continue;
        if (execution.iep_bitmap && static_cast<int>(loop) <= plan.counting.iep_depth) {
            const auto &bitmap = *execution.iep_bitmap;
            if (bitmap.universe_set) {
                if (execution.sets.at(*bitmap.universe_set).depth < static_cast<int>(loop) &&
                    *bitmap.universe_set != plan.logical.iter_set.at(loop - 1).id)
                    capture(out.captured_sets, *bitmap.universe_set);
            } else if (bitmap.anchor_depth < static_cast<int>(loop))
                out.captured_adjacencies.insert(bitmap.anchor_depth);
        }
        if (plan.context.config.pruningType != PruningType::None) {
            for (int dep = loop; dep < plan.logical.p_size - 1; ++dep)
                for (const auto &mg : plan.auxiliary.mg_used.at(dep))
                    if (mg.loop_depth() < static_cast<int>(loop))
                        capture(out.captured_minigraphs, mg.id);
            for (const auto &mg : plan.auxiliary.mg_ops.at(loop)) {
                const auto parent = plan.get_parent_mg(mg);
                if (parent && parent->loop_depth() < static_cast<int>(loop))
                    capture(out.captured_minigraphs, parent->id);
            }
        }
        for (const auto &op : plan.logical.set_ops.at(loop)) {
            const auto parent = plan.get_parent_vset(op, loop - 1);
            if (parent && parent->id != plan.logical.iter_set.at(loop - 1).id)
                capture(out.captured_sets, parent->id);
        }
        for (int depth = loop; depth < plan.logical.p_size - 1; ++depth)
            for (const auto &op : plan.logical.set_ops.at(depth))
                if (!plan.get_parent_vset(op, loop - 1))
                    for (size_t prior = 0; prior < loop; ++prior)
                        out.captured_adjacencies.insert(prior);
    }
}
} // namespace minigraph
