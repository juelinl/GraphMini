#include "compiler/representation.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace minigraph {
DomainAnalysis analyze_domains(const ScheduledConstraints &logical) {
    const int n = logical.p_size;
    if (n < 1 || n > MAX_PATTERN_SIZE || logical.adjacency.size() != static_cast<size_t>(n * n))
        throw std::logic_error("Invalid scheduled adjacency for domain analysis");
    DomainAnalysis out;
    for (const auto &level : logical.set_ops) {
        for (const auto &set : level) {
            if (set.loop_depth() < 0 || set.loop_depth() >= n || set.id < 0)
                throw std::logic_error("Invalid set domain definition");
            SetDomain domain;
            // Intersections establish containment; bounds, removals, and
            // nonedge differences preserve it. Nonedges never establish it.
            for (int anchor = 0; anchor <= set.loop_depth(); ++anchor) {
                if (set.is_edge(anchor)) {
                    domain.neighborhood_anchors.push_back(anchor);
                    out.universes.emplace(anchor, NeighborhoodUniverseIR{anchor});
                }
            }
            if (!out.sets.emplace(set.id, std::move(domain)).second)
                throw std::logic_error("Duplicate set domain definition");
        }
    }
    // Keep all legal anchors. A later cost model chooses one and accounts for
    // conversion/restricted-neighborhood costs. Require at least two suffix vertices.
    for (int depth = 0; depth < n - 2; ++depth) {
        for (int anchor = 0; anchor <= depth; ++anchor) {
            SharedUniverseRegion region{depth, anchor, {}};
            for (int vertex = depth + 1; vertex < n; ++vertex) {
                if (logical.adjacency[anchor * n + vertex] != '1')
                    break;
                region.remaining_vertices.push_back(vertex);
            }
            if (region.remaining_vertices.size() == static_cast<size_t>(n - depth - 1)) {
                out.universes.emplace(anchor, NeighborhoodUniverseIR{anchor});
                out.regions.push_back(std::move(region));
            }
        }
    }
    return out;
}

RepresentationPlan select_representations(const ScheduledConstraints &logical,
                                          const DomainAnalysis &domains) {
    RepresentationPlan out;
    for (const auto &level : logical.set_ops)
        for (const auto &set : level)
            out.sets.emplace(set.id, SetRepresentation{});
    verify_representations(logical, domains, out);
    return out;
}

void verify_representations(const ScheduledConstraints &logical, const DomainAnalysis &domains,
                            const RepresentationPlan &representations) {
    // Re-derive proofs rather than trusting annotations supplied by a pass.
    const auto expected = analyze_domains(logical);
    if (domains.sets.size() != expected.sets.size() ||
        domains.universes.size() != expected.universes.size() ||
        domains.regions.size() != expected.regions.size() ||
        representations.sets.size() != expected.sets.size())
        throw std::logic_error("Domain/representation coverage mismatch");
    for (const auto &[id, universe] : expected.universes)
        if (!domains.universes.count(id) ||
            domains.universes.at(id).anchor_depth != universe.anchor_depth)
            throw std::logic_error("Invalid universe identity");
    for (size_t i = 0; i < expected.regions.size(); ++i) {
        const auto &a = domains.regions[i];
        const auto &b = expected.regions[i];
        if (a.entry_depth != b.entry_depth || a.anchor_depth != b.anchor_depth ||
            a.remaining_vertices != b.remaining_vertices)
            throw std::logic_error("Unproved shared universe region");
    }
    for (const auto &[id, domain] : expected.sets) {
        if (!domains.sets.count(id) ||
            domains.sets.at(id).neighborhood_anchors != domain.neighborhood_anchors)
            throw std::logic_error("Unproved candidate-set containment");
        const auto &representation = representations.sets.at(id);
        if (representation.storage != SetStorage::SortedArray || representation.universe)
            throw std::logic_error(
                "Bitmap lowering is not implemented; only unconverted sorted arrays are supported");
    }
}

std::string dump_domains(const DomainAnalysis &domains, const RepresentationPlan &representations) {
    std::ostringstream out;
    for (const auto &[id, universe] : domains.universes)
        out << "universe" << id << " = N(vertex" << universe.anchor_depth
            << ") order=global-id lifetime=anchor-binding\n";
    for (const auto &[id, domain] : domains.sets) {
        out << "set" << id << " subset-of:";
        for (int anchor : domain.neighborhood_anchors)
            out << " universe" << anchor;
        out << " storage="
            << (representations.sets.at(id).storage == SetStorage::SortedArray ? "sorted-array"
                                                                               : "bitmap")
            << '\n';
    }
    for (const auto &region : domains.regions) {
        out << "shared-universe-candidate @depth" << region.entry_depth << " universe"
            << region.anchor_depth << " suffix:";
        for (int vertex : region.remaining_vertices)
            out << " vertex" << vertex;
        out << '\n';
    }
    return out.str();
}
} // namespace minigraph
