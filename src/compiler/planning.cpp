#include "codegen.h"
#include "ir.h"
#include "logging.h"
#include "timer.h"

#include "compiler/planning.h"
#include "compilation_profile.h"
#include "graphmini_scheduler.hpp"
#include "graphpi_scheduler.hpp"
#include "typedef.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <numeric>
#include <set>
#include <sstream>
#include <utility>
namespace minigraph {
namespace {
template <typename Scheduler> ScheduleResult capture_schedule_result(Scheduler &scheduler) {
    ScheduleResult result;
    result.adj_mat = scheduler.get_adj_mat_str();
    result.matching_order = scheduler.get_matching_order();
    result.restrict_pair = scheduler.restrict_pair;
    result.iep_num = scheduler.get_in_exclusion_optimize_num();
    result.iep_groups = scheduler.in_exclusion_optimize_group;
    result.iep_vals = scheduler.in_exclusion_optimize_val;
    result.iep_redundancy = scheduler.get_in_exclusion_optimize_redundancy();
    return result;
}

std::string format_query_vertex(int vertex) { return fmt::format("n{}", vertex); }

std::string format_schedule_vertex(int vertex) { return fmt::format("v{}", vertex); }

std::string format_generated_schedule(const std::vector<int> &matching_order) {
    if (matching_order.empty()) {
        return "Generated Schedule: unavailable";
    }

    std::ostringstream out;
    out << "Generated Schedule:";
    for (size_t i = 0; i < matching_order.size(); ++i) {
        out << "\n  " << format_query_vertex(matching_order[i]) << " -> "
            << format_schedule_vertex(static_cast<int>(i));
    }
    return out.str();
}

std::string format_canonicality_constraints(const std::vector<std::pair<int, int>> &restrict_pair,
                                            int pattern_size) {
    std::vector<std::vector<int>> grouped_constraints(static_cast<size_t>(pattern_size));
    for (const auto &[lhs, rhs] : restrict_pair) {
        grouped_constraints[static_cast<size_t>(rhs)].push_back(lhs);
    }

    std::ostringstream out;
    out << "Canonicality Constraints:";
    bool has_constraints = false;
    for (int vertex = 0; vertex < pattern_size; ++vertex) {
        auto &checks = grouped_constraints[static_cast<size_t>(vertex)];
        if (checks.empty()) {
            continue;
        }

        has_constraints = true;
        std::sort(checks.begin(), checks.end());
        out << "\n  " << format_schedule_vertex(vertex) << ": ";
        for (size_t i = 0; i < checks.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << format_schedule_vertex(checks[i]) << " > " << format_schedule_vertex(vertex);
        }
    }

    if (!has_constraints) {
        out << "\n  none";
    }
    return out.str();
}

std::string format_scheduled_adjacency_lists(const std::string &adj_mat, int pattern_size) {
    std::ostringstream out;
    out << "Scheduled Adjacency Lists:";
    for (int row = 0; row < pattern_size; ++row) {
        out << "\n  " << format_schedule_vertex(row) << ": ";

        std::vector<int> unmatched_neighbors;
        for (int col = row + 1; col < pattern_size; ++col) {
            if (adj_mat[static_cast<size_t>(row * pattern_size + col)] != '1') {
                continue;
            }
            unmatched_neighbors.push_back(col);
        }

        std::sort(unmatched_neighbors.begin(), unmatched_neighbors.end());
        if (unmatched_neighbors.empty()) {
            out << "none";
            continue;
        }

        for (size_t i = 0; i < unmatched_neighbors.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << format_schedule_vertex(unmatched_neighbors[i]);
        }
    }
    return out.str();
}

ScheduleResult schedule_pattern(const std::string &adj_mat, int p_size, const CodeGenConfig &config,
                                const MetaData &meta) {
    switch (config.schedulerType) {
    case SchedulerType::GraphPi: {
        GraphPiScheduler scheduler{};
        scheduler.get_schedule(adj_mat.c_str(), p_size, meta.num_vertex, meta.num_edge, meta.num_triangle,
                               meta.scheduler_avg_degree, PerfModelType::graphpi);
        return capture_schedule_result(scheduler);
    }
    case SchedulerType::GraphZero: {
        GraphPiScheduler scheduler{};
        scheduler.get_schedule(adj_mat.c_str(), p_size, meta.num_vertex, meta.num_edge, meta.num_triangle,
                               meta.scheduler_avg_degree, PerfModelType::graphzero);
        return capture_schedule_result(scheduler);
    }
    case SchedulerType::GraphMini: {
        GraphMiniScheduler scheduler{};
        scheduler.get_schedule(adj_mat.c_str(), p_size, meta.num_vertex, meta.num_edge, meta.num_triangle);
        return capture_schedule_result(scheduler);
    }
    }

    GraphMiniScheduler scheduler{};
    scheduler.get_schedule(adj_mat.c_str(), p_size, meta.num_vertex, meta.num_edge, meta.num_triangle);
    return capture_schedule_result(scheduler);
}
} // namespace
inline int VEC_INDEX(int i, int j, int p_size) { return i * p_size + j; };
std::string restricts_to_str(std::vector<std::pair<int, int>> &pair_vec, int p_size) {
    std::string mat;
    mat.resize(p_size * p_size, '0');

    for (auto pair : pair_vec) {
        mat.at(VEC_INDEX(pair.second, pair.first, p_size)) = '1';
    }
    for (int v1 = 0; v1 < p_size; v1++) {
        std::vector<int> greater_than_v1;
        for (int v1_plus = 0; v1_plus < p_size; ++v1_plus) {
            if (mat.at(VEC_INDEX(v1, v1_plus, p_size)) == '1') {
                greater_than_v1.push_back(v1_plus);
            }
        }

        for (auto &pair : pair_vec) {
            if (pair.first == v1) {
                int less_than_v1 = pair.second;
                for (auto v1_plus : greater_than_v1) {
                    mat.at(VEC_INDEX(less_than_v1, v1_plus, p_size)) = '1';
                }
            }
        }
    }

    return mat;
}

int get_pattern_size(const std::string &adj_mat) {
    int pattern_size = (int)sqrt(adj_mat.size());
    if (pattern_size * pattern_size != adj_mat.size() || pattern_size < 3)
        LOG(FATAL) << "Invalid adj matrix size, it must be a square number";
    return pattern_size;
}

EdgeIR ToEdgeIR(const std::string &adj_mat, int vid) {
    if (vid == 0)
        return {};
    int p_size = get_pattern_size(adj_mat);
    EdgeIR out;
    for (int j = 0; j < p_size; j++) {
        out[j] = adj_mat.at(VEC_INDEX(vid, j, p_size)) == '1';
    }
    return out;
}

EdgeRestrictIR ToRestrictIR(const std::string &res_mat, int vid) { return ToEdgeIR(res_mat, vid); };

ScheduledPlan build_plan(const std::string &_adj_mat, CodeGenConfig config, MetaData meta) {
    Timer t;
    int p_size = get_pattern_size(_adj_mat);
    ScheduleResult schedule;
    {
        CompilationStage stage("scheduling");
        schedule = schedule_pattern(_adj_mat, p_size, config, meta);
    }
    std::string adj_mat = schedule.adj_mat;
    std::string res_mat = restricts_to_str(schedule.restrict_pair, p_size);
    {
    CompilationStage stage("schedule_diagnostics");
    LOG(MSG) << format_generated_schedule(schedule.matching_order);
    LOG(MSG) << format_scheduled_adjacency_lists(adj_mat, p_size);
    LOG(MSG) << format_canonicality_constraints(schedule.restrict_pair, p_size);
    LOG(MSG) << "Scheduling Time: " << ToReadableDuration(t.Passed());
    }
    t.Reset();
    CompilationStage stage("base_ir");
    PlanIR out;
    out.config = config;
    int max_dep = p_size - 1;
    std::vector<EdgeIR> edge_ir_vec(p_size);
    std::vector<EdgeRestrictIR> res_ir_vec(p_size);

    std::vector<std::vector<VertexSetIR>> set_ops(max_dep);
    std::vector<VertexSetIR> iter_set(max_dep);

    for (int vid = 1; vid < p_size; vid++) {
        edge_ir_vec.at(vid) = ToEdgeIR(adj_mat, vid);
        res_ir_vec.at(vid) = ToRestrictIR(res_mat, vid);
    }

    for (int dep = 0; dep < max_dep; dep++) {
        for (int vid = dep + 1; vid < p_size; vid++) {
            const EdgeIR &e = edge_ir_vec.at(vid);
            const EdgeRestrictIR &r = res_ir_vec.at(vid);
            VertexSetIR v_ir = VertexSetIR(e, r, dep, config.adjMatType);
            if (v_ir.edge_num() > 0)
                set_ops.at(dep).push_back(v_ir);
        }

        // next vertex set to iterate through
        int iter_vid = dep + 1;
        const EdgeIR &e = edge_ir_vec.at(iter_vid);
        const EdgeRestrictIR &r = res_ir_vec.at(iter_vid);
        VertexSetIR v_iter_ir = VertexSetIR(e, r, dep, config.adjMatType);
        CHECK(v_iter_ir.edge_num() > 0) << "Invalid schedule: The set of vertex to iterate potentially "
                                           "contains all vertices in the graph";
        iter_set.at(dep) = v_iter_ir;
    }
    // remove duplicated vertexIR
    int next_id = 0;
    for (auto &ops : set_ops) {
        std::sort(ops.begin(), ops.end());
        ops.erase(std::unique(ops.begin(), ops.end()), ops.end());
        for (auto &op : ops) {
            op.id = next_id++;
        }
    }

    for (int dep = 0; dep < max_dep; dep++) {
        const auto &ops = set_ops.at(dep);
        VertexSetIR iter_vs = *std::find(ops.begin(), ops.end(), iter_set.at(dep));
        iter_set.at(dep) = iter_vs;
        CHECK(iter_vs.has_id()) << "Invalid VertexSetIR (id = -1)";
    }
    out.p_size = p_size;
    out.set_ops = set_ops;
    out.iter_set = iter_set;
    out.meta = meta;
    return {std::move(out), std::move(schedule)};
};

} // namespace minigraph
