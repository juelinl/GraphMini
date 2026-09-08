#include "compiler/planning.h"
#include <algorithm>
namespace minigraph {
PlanIR create_plan_mg(const PlanIR &_plan, const CodeGenConfig &_config) {
    PlanIR out = _plan;
    int max_dep = std::min(out.logical.p_size - 2, out.logical.p_size - out.counting.iep_num - 1);
    out.auxiliary.mg_ops.resize(out.logical.p_size);
    for (int comp_dep = max_dep; comp_dep >= 2; --comp_dep) {
        const std::vector<VertexSetIR> &vset_vec = out.logical.set_ops.at(comp_dep);
        const VertexSetIR &iter = out.logical.iter_set.at(comp_dep - 1);
        for (const VertexSetIR &vset : vset_vec) {
            if (_config.adjMatType != AdjMatType::VertexInduced && vset.is_edge(comp_dep) == false) {
                continue; // EdgeInduced matching does not require set subtraction
            }

            for (int prune_dep = 0; prune_dep <= comp_dep - 2; prune_dep++) {
                std::optional<VertexSetIR> intersect = out.get_parent_vset(vset, prune_dep);
                std::optional<VertexSetIR> vertices = out.get_parent_vset(iter, prune_dep);
                if (intersect.has_value() && vertices.has_value()) {
                    MiniGraphIR mg_op(vertices.value(), intersect.value());
                    if (_config.pruningType == PruningType::Static) {
                        VertexSetIR next_vertices = out.logical.iter_set.at(prune_dep); // next iteration
                        for (const auto &next_intersect : out.logical.set_ops.at(prune_dep + 1)) {
                            if (mg_op.computed(next_vertices, next_intersect)) {
                                out.auxiliary.mg_ops.at(prune_dep).push_back(mg_op);
                                break;
                            }
                        }
                    } else {
                        out.auxiliary.mg_ops.at(prune_dep).push_back(mg_op);
                    }
                } // find a valid minigraph
            }
        }
    }
    // remove duplicates
    int next_id = 0;
    for (auto &mg_op : out.auxiliary.mg_ops) {
        std::sort(mg_op.begin(), mg_op.end());
        mg_op.erase(std::unique(mg_op.begin(), mg_op.end()), mg_op.end());
        for (auto &mg : mg_op) {
            mg.id = next_id++;
        }
    }
    std::vector<bool> IndeedUsed(next_id, false);
    out.auxiliary.mg_used.resize(out.logical.p_size);
    out.auxiliary.mg_bounded.clear();
    out.auxiliary.mg_bounded.resize(next_id, true);
    for (int dep = 1; dep <= max_dep; dep++) {
        const auto &ops = out.logical.set_ops.at(dep);
        std::vector<MiniGraphIR> mg_used_vec;
        for (const VertexSetIR &op : ops) {
            std::optional<MiniGraphIR> mg_used = out.get_parent_mg(op);
            if (mg_used.has_value()) {
                IndeedUsed.at(mg_used->id) = true;
                bool to_add = true;
                for (const auto &mg : mg_used_vec) {
                    if (mg.id == mg_used->id)
                        to_add = false;
                }
                if (to_add) {
                    mg_used_vec.push_back(mg_used.value());
                    out.auxiliary.mg_bounded.at(mg_used->id) =
                        out.auxiliary.mg_bounded.at(mg_used->id) && op.is_restricted(op.loop_depth());
                }
            }
        }
        out.auxiliary.mg_used.at(dep) = mg_used_vec;
    }

    for (int dep = max_dep; dep >= 0; dep--) {
        for (const MiniGraphIR &child : out.auxiliary.mg_ops.at(dep)) {
            if (IndeedUsed.at(child.id)) {
                std::optional<MiniGraphIR> parent = out.get_parent_mg(child);
                if (parent.has_value()) {
                    IndeedUsed.at(parent->id) = true;
                    out.auxiliary.mg_bounded.at(parent->id) =
                        out.auxiliary.mg_bounded.at(parent->id) && out.auxiliary.mg_bounded.at(child.id);
                }
            }
        }
    }
    for (auto &mg_op : out.auxiliary.mg_ops) {
        mg_op.erase(std::remove_if(mg_op.begin(), mg_op.end(),
                                   [&IndeedUsed](const MiniGraphIR &mg) {
                                       return mg.id < 0 || static_cast<size_t>(mg.id) >= IndeedUsed.size() ||
                                              !IndeedUsed.at(static_cast<size_t>(mg.id));
                                   }),
                    mg_op.end());
    }
    return out;
};

} // namespace minigraph
