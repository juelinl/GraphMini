#include "common/meta.h"
#include "compiler/planning.h"
namespace minigraph {
PlanIR compile_edge_induced_iep(const std::string &query, CodeGenConfig config, MetaData meta) {
    config.adjMatType = AdjMatType::EdgeInducedIEP;
    return compile_edge_induced_iep(build_plan(query, config, meta));
}
PlanIR compile_edge_induced_iep(ScheduledPlan scheduled) {
    auto &out = scheduled.plan;
    const auto &schedule = scheduled.schedule;
    const int p_size = out.logical.p_size;
    out.counting.iep_num = schedule.iep_num;
    out.counting.iep_depth = p_size - out.counting.iep_num - 1;
    out.counting.iep_groups = schedule.iep_groups;
    out.counting.iep_vals = schedule.iep_vals;
    out.counting.iep_redundancy = schedule.iep_redundancy;

    if (out.counting.iep_num > 1) {
        for (int vset_id = 0; vset_id < out.counting.iep_num; ++vset_id) {
            int iter_depth = vset_id + out.counting.iep_depth;
            std::optional<VertexSetIR> vset;
            VertexSetIR child = out.logical.iter_set.at(iter_depth);
            for (VertexSetIR parent : out.logical.set_ops.at(out.counting.iep_depth)) {
                if (parent.same_iep_computation(child))
                    vset = parent;
            }
            assert(vset.has_value());
            out.counting.iep_set.push_back(vset.value());
        }
    }

    return std::move(out);
}
} // namespace minigraph
