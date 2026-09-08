#pragma once
#include "compiler/ir.h"
#include <map>
#include <optional>
#include <string>

namespace minigraph {
// Universe identity is a matched loop depth, not a runtime graph vertex ID.
// Each loop binding creates a fresh universe ordered by global vertex ID.
struct NeighborhoodUniverseIR {
    int anchor_depth;
};
struct SetDomain {
    // Each entry independently proves S is a subset of N(vertex[anchor]).
    std::vector<int> neighborhood_anchors;
};
struct SharedUniverseRegion {
    int entry_depth;
    int anchor_depth;
    std::vector<int> remaining_vertices; // scheduled pattern positions
};
struct DomainAnalysis {
    std::map<int, NeighborhoodUniverseIR> universes;
    std::map<int, SetDomain> sets;
    std::vector<SharedUniverseRegion> regions;
};
enum class SetStorage { SortedArray, UniverseBitmap };
struct SetRepresentation {
    SetStorage storage{SetStorage::SortedArray};
    std::optional<int> universe;
};
struct RepresentationPlan {
    std::map<int, SetRepresentation> sets;
};

// These analyses deliberately cannot inspect pruning, scheduling policy, or
// graph statistics. Legality facts are separate from profitability decisions.
DomainAnalysis analyze_domains(const ScheduledConstraints &logical);
RepresentationPlan select_representations(const ScheduledConstraints &logical,
                                          const DomainAnalysis &domains);
void verify_representations(const ScheduledConstraints &logical, const DomainAnalysis &domains,
                            const RepresentationPlan &representations);
std::string dump_domains(const DomainAnalysis &domains, const RepresentationPlan &representations);
} // namespace minigraph
