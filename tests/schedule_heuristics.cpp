#include "graphmini_scheduler.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace minigraph;
void require(bool condition) {
    if (!condition)
        throw std::logic_error("Scheduling heuristic regression");
}
int main() {
    int checked = 0;
    // Every connected labeled four-vertex pattern, including asymmetric labels.
    for (int mask = 0; mask < 64; ++mask) {
        std::string a(16, '0');
        int bit = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j, ++bit)
                if (mask & (1 << bit))
                    a[i * 4 + j] = a[j * 4 + i] = '1';
        const auto current = heuristic_candidates(a, 4, ScheduleHeuristic::Current);
        if (current.empty())
            continue;
        std::vector<int> expected(4), order{0, 1, 2, 3};
        do {
            bool valid = true;
            std::vector<int> outgoing(4);
            for (int i = 0; i < 4; ++i) {
                bool connected = i == 0;
                for (int j = 0; j < i; ++j)
                    connected |= a[order[i] * 4 + order[j]] == '1';
                valid &= connected;
                for (int j = i + 1; j < 4; ++j)
                    outgoing[i] += a[order[i] * 4 + order[j]] == '1';
            }
            if (valid)
                expected = std::max(expected, outgoing);
        } while (std::next_permutation(order.begin(), order.end()));
        for (auto policy : {ScheduleHeuristic::Current, ScheduleHeuristic::Outgoing,
                            ScheduleHeuristic::BitmapBalanced, ScheduleHeuristic::IepFirst}) {
            GraphMiniScheduler scheduler;
            scheduler.get_schedule(a.c_str(), 4, 0, 0, 0, policy);
            const auto result = scheduler.get_adj_mat_str();
            if (policy == ScheduleHeuristic::Current)
                require(result == current.front().adjacency);
            if (policy == ScheduleHeuristic::Outgoing)
                require(outgoing_profile(result, 4) == expected);
            // On a host equal to the scheduled pattern, accept one automorphism.
            int accepted = 0;
            order = {0, 1, 2, 3};
            do {
                bool valid = true;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        valid &= result[i * 4 + j] == result[order[i] * 4 + order[j]];
                for (auto [first, second] : scheduler.restrict_pair)
                    valid &= order[first] > order[second];
                accepted += valid;
            } while (std::next_permutation(order.begin(), order.end()));
            require(accepted == 1);
            ++checked;
        }
    }
    require(checked == 38 * 4);
    require(supported_iep_width("0111100010001000", 4) == 2);
    require(supported_iep_width("0111101111011110", 4) == 0);
    // Atlas 660: outgoing-first misses a legal three-vertex independent suffix.
    const std::string iep_counterexample = "0011010001100111011001110100001101010001000100000";
    GraphMiniScheduler outgoing, iep;
    outgoing.get_schedule(iep_counterexample.c_str(), 7, 0, 0, 0, ScheduleHeuristic::Outgoing);
    iep.get_schedule(iep_counterexample.c_str(), 7, 0, 0, 0, ScheduleHeuristic::IepFirst);
    require(supported_iep_width(outgoing.get_adj_mat_str(), 7) == 0);
    require(iep.get_in_exclusion_optimize_num() == 2);
    require(supported_iep_width(iep.get_adj_mat_str(), 7) == 2);
    require(bitmap_opportunity_entry("0111101111011110", 4) == 0);
    require(bitmap_opportunity_entry("0101101001011010", 4) == -1);
    // K(2,4): the shortlist should move the second A vertex before the B suffix.
    const std::string bipartite = "000011000011000011000011111100111100";
    const auto baseline = heuristic_candidates(bipartite, 6, ScheduleHeuristic::Current);
    const auto balanced = heuristic_candidates(bipartite, 6, ScheduleHeuristic::BitmapBalanced);
    require(bitmap_opportunity_entry(baseline.front().adjacency, 6) == -1);
    for (const auto &c : balanced)
        require(bitmap_opportunity_entry(c.adjacency, 6) == 2);
    // Root-universe cases have no earlier opportunity, so retain current order.
    const std::string star = "0111100010001000";
    require(heuristic_candidates(star, 4, ScheduleHeuristic::Current).front().adjacency ==
            heuristic_candidates(star, 4, ScheduleHeuristic::BitmapBalanced).front().adjacency);
    bool rejected = false;
    try {
        heuristic_candidates(std::string(81, '1'), 9, ScheduleHeuristic::Outgoing);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected);
    std::cout << "Validated " << checked << " schedules and experimental size guard\n";
}
