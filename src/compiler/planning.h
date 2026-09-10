#pragma once
#include <string>
#include "common/meta.h"
#include "compiler/codegen.h"
#include "compiler/ir.h"
#include <utility>
namespace minigraph {
struct ScheduleResult {
    std::string adj_mat;
    std::vector<int> matching_order;
    std::vector<std::pair<int, int>> restrict_pair;
    int iep_num{0};
    std::vector<std::vector<std::vector<int>>> iep_groups;
    std::vector<int> iep_vals;
    long long iep_redundancy{1};
};

struct ScheduledPlan {
    PlanIR plan;
    ScheduleResult schedule;
};
ScheduledPlan build_plan(const std::string &, CodeGenConfig, MetaData);
ScheduleResult schedule_query(const std::string &, CodeGenConfig, MetaData);
ScheduledPlan build_plan(const std::string &, CodeGenConfig, MetaData, ScheduleResult);
PlanIR create_plan_mg(const PlanIR &, const CodeGenConfig &);
PlanIR compile_vertex_induced(const std::string &, CodeGenConfig, MetaData);
PlanIR compile_edge_induced(const std::string &, CodeGenConfig, MetaData);
PlanIR compile_edge_induced_iep(const std::string &, CodeGenConfig, MetaData);
PlanIR compile_edge_induced_iep(ScheduledPlan);
} // namespace minigraph
