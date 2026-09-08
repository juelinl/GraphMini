#include "compiler/execution_ir.h"
#include <algorithm>

namespace minigraph {
namespace {
bool shares_indices(const MiniGraphIR &mg, const VertexSetIR &iter) {
    const auto &vertices = mg.m_vertices;
    if (iter.loop_depth() != vertices.loop_depth())
        return false;
    for (int i = 0; i <= iter.loop_depth(); ++i)
        if (iter.is_edge(i) != vertices.is_edge(i))
            return false;
    return true;
}
bool eager_next_use(const PlanIR &plan, const MiniGraphIR &mg) {
    const auto &next = plan.logical.iter_set.at(mg.loop_depth());
    if (!(next == mg.m_vertices))
        return false;
    for (const auto &op : plan.logical.set_ops.at(mg.loop_depth() + 1))
        if (mg.computed(next, op))
            return true;
    return false;
}
ReuseEstimate estimate_visits(const PlanIR &plan, const MiniGraphIR &mg, int depth, int uses) {
    ReuseEstimate out{plan.logical.iter_set.at(mg.loop_depth()).id, {}, uses};
    for (int dep = mg.loop_depth() + 1; dep < depth; ++dep) {
        const auto &iter = plan.logical.iter_set.at(dep);
        const auto parent = plan.get_parent_vset(iter, mg.loop_depth());
        if (parent) {
            const double p1 = 1.0 * plan.context.meta.num_edge / plan.context.meta.num_vertex / plan.context.meta.num_vertex;
            const double p2 = 1.0 * plan.context.meta.num_triangle * 6 * plan.context.meta.num_vertex /
                              plan.context.meta.num_edge / plan.context.meta.num_edge;
            double rate = 1.0;
            for (int adj = mg.loop_depth(); adj < iter.loop_depth(); ++adj)
                rate *= plan.logical.iter_set.at(adj).share_at_least_one_parent_node(*parent) ? p2 : p1;
            out.factors.push_back({parent->id, rate});
        } else
            out.factors.push_back({{}, 1.0 * plan.context.meta.num_edge / plan.context.meta.num_vertex});
    }
    return out;
}
} // namespace

void lower_minigraphs(const PlanIR &plan, ExecutionIR &execution) {
    for (const auto &level : plan.auxiliary.mg_ops) {
        for (const auto &mg : level) {
            const auto parent = plan.get_parent_mg(mg);
            const bool eager = eager_next_use(plan, mg);
            MiniGraphExecution out{mg.id,
                                   mg.loop_depth(),
                                   mg.vset_id(),
                                   mg.vint_id(),
                                   plan.logical.iter_set.at(mg.loop_depth()).id,
                                   parent ? std::optional<int>(parent->id) : std::nullopt,
                                   eager,
                                   plan.is_bounded(mg),
                                   false, // Existing policy: serial MiniGraph construction.
                                   plan.context.config.pruningType == PruningType::CostModel && !eager,
                                   {},
                                   {}};
            for (const auto &iter : plan.logical.iter_set)
                out.direct_indices.emplace(iter.id, shares_indices(mg, iter));
            if (out.estimate_reuse) {
                const int last = std::min(plan.logical.p_size - 2, plan.logical.p_size - plan.counting.iep_num - 1);
                for (int depth = mg.loop_depth() + 2; depth <= last; ++depth) {
                    int uses = 0;
                    for (const auto &op : plan.logical.set_ops.at(depth)) {
                        const auto selected = plan.get_parent_mg(op);
                        if (selected && (mg.is_superset_of(*selected) || mg == *selected))
                            ++uses;
                    }
                    if (uses)
                        out.reuse.push_back(estimate_visits(plan, mg, depth, uses));
                }
            }
            execution.minigraphs.emplace(mg.id, std::move(out));
        }
    }
}
} // namespace minigraph
