#include "compiler/planning.h"
namespace minigraph {
PlanIR compile_edge_induced_iep(const std::string &query, CodeGenConfig config, MetaData meta) {
    config.adjMatType = AdjMatType::EdgeInducedIEP;
    auto scheduled = build_plan(query, config, meta);
    auto &out = scheduled.plan;
    const auto &schedule = scheduled.schedule;
    const int p_size = out.p_size;
    out.iep_num = schedule.iep_num;
    out.iep_depth = p_size - out.iep_num - 1;
    out.iep_groups = schedule.iep_groups;
    out.iep_vals = schedule.iep_vals;
    out.iep_redundancy = schedule.iep_redundancy;

    if (out.iep_num > 1) {
        for (int vset_id = 0; vset_id < out.iep_num; ++vset_id) {
            int iter_depth = vset_id + out.iep_depth;
            std::optional<VertexSetIR> vset;
            VertexSetIR child = out.iter_set.at(iter_depth);
            for (VertexSetIR parent : out.set_ops.at(out.iep_depth)) {
                if (parent.same_iep_computation(child))
                    vset = parent;
            }
            assert(vset.has_value());
            out.iep_set.push_back(vset.value());
        }
    }

    return std::move(out);
}
} // namespace minigraph
