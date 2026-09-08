#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include "compiler/scheduling/schedule_heuristics.hpp"
#include <iostream>
#include <stdexcept>

using namespace minigraph;
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    int n;
    std::string query;
    while (std::cin >> n >> query) {
        for (auto scheduler : {SchedulerType::GraphMini, SchedulerType::Outgoing,
                               SchedulerType::IepFirst}) {
            CodeGenConfig config;
            config.schedulerType = scheduler;
            config.adjMatType = EdgeInducedIEP;
            config.pruningType = PruningType::None;
            config.parType = ParallelType::OpenMP;
            const auto scheduled = build_plan(query, config, meta);
            const auto plan = compile_edge_induced_iep(query, config, meta);
            const auto ir = lower_execution(plan);
            const auto &s = scheduled.schedule;
            const int width = plan.counting.iep_num > 1 ? plan.counting.iep_num : 0;
            if (width != supported_iep_width(s.adj_mat, n) ||
                (width > 0) != !ir.iep.empty())
                throw std::logic_error("Predicted IEP width disagrees with physical lowering");
            std::cout << static_cast<int>(scheduler) << '\t' << s.adj_mat << '\t';
            for (int v : s.matching_order)
                std::cout << v << ',';
            std::cout << '\t';
            for (auto [a, b] : s.restrict_pair)
                std::cout << a << ':' << b << ',';
            std::cout << '\t' << width << '\t' << (width ? plan.counting.iep_depth : -1)
                      << '\t' << ir.iep.size() << '\t' << plan.counting.iep_redundancy << '\n';
        }
        std::cout.flush();
    }
}
