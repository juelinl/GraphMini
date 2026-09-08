#pragma once
#include <string>
#include <vector>

namespace minigraph {
enum class ScheduleHeuristic { Current, Outgoing, BitmapBalanced, IepFirst };
struct ScheduleCandidate {
    std::vector<int> order;
    std::string adjacency;
};
// Experimental topology-only ranking. Canonicality breaks ties in the scheduler.
// BitmapBalanced keeps the best eight distinct current-heuristic structures.
// It preserves the current order unless that shortlist offers an earlier universe.
std::vector<ScheduleCandidate> heuristic_candidates(const std::string &adjacency, int size,
                                                    ScheduleHeuristic policy);
std::vector<int> outgoing_profile(const std::string &adjacency, int size);
// Scope after matching entry; -1 means no anchor shared by a >=3-vertex suffix.
// This is an opportunity estimate, not a substitute for physical IR verification.
int bitmap_opportunity_entry(const std::string &adjacency, int size);
// Current backend reserves one suffix vertex; widths <=1 do not activate IEP.
int supported_iep_width(const std::string &adjacency, int size);
} // namespace minigraph
