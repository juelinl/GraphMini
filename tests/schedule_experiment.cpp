// Streaming schedule/physical-plan inspection; no dynamic query compilation.
#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include "schedule_heuristics.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace minigraph;
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    int n;
    std::string query;
    while (std::cin >> n >> query) {
        for (auto scheduler :
             {SchedulerType::GraphMini, SchedulerType::Outgoing, SchedulerType::BitmapBalanced}) {
            CodeGenConfig config;
            config.schedulerType = scheduler;
            config.pruningType = PruningType::None;
            config.parType = ParallelType::OpenMP;
            config.bitmap = true;
            const auto result = build_plan(query, config, meta);
            const auto &s = result.schedule;
            const auto ir = lower_execution(result.plan);
            if (s.adj_mat.size() != query.size() || s.matching_order.size() != size_t(n))
                throw std::logic_error("Schedule size mismatch");
            auto sorted = s.matching_order;
            std::sort(sorted.begin(), sorted.end());
            for (int i = 0; i < n; ++i) {
                if (sorted[i] != i)
                    throw std::logic_error("Invalid matching permutation");
                for (int j = 0; j < n; ++j)
                    if (s.adj_mat[i * n + j] != query[s.matching_order[i] * n + s.matching_order[j]])
                        throw std::logic_error("Schedule changed query semantics");
            }
            // Independently enumerated current score must match the existing DFS.
            if (scheduler == SchedulerType::GraphMini &&
                heuristic_candidates(query, n, ScheduleHeuristic::Current).front().adjacency !=
                    s.adj_mat)
                throw std::logic_error("Shortlist ranking disagrees with current scheduler");
            std::cout << static_cast<int>(scheduler) << '\t' << s.adj_mat << '\t';
            for (int vertex : s.matching_order)
                std::cout << vertex << ',';
            std::cout << '\t';
            for (auto [a, b] : s.restrict_pair)
                std::cout << a << ':' << b << ',';
            std::cout << '\t' << bitmap_opportunity_entry(s.adj_mat, n) << '\t'
                      << (ir.bitmap_region ? ir.bitmap_region->entry_depth : -1) << '\t'
                      << (ir.bitmap_region ? ir.bitmap_region->anchor_depth : -1) << '\t'
                      << ir.bitmap_reason << '\n';
        }
        std::cout.flush();
    }
}
