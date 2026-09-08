#include "compiler/representation.h"
#include <iostream>
#include <stdexcept>

using namespace minigraph;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected, "Accepted invalid representation facts");
}
ScheduledConstraints logical(const std::string &matrix, AdjMatType mode) {
    ScheduledConstraints out;
    out.p_size = 4;
    out.adjacency = matrix;
    out.matching_order = {0, 1, 2, 3};
    out.set_ops.resize(3);
    int id = 0;
    for (int depth = 0; depth < 3; ++depth)
        for (int vertex = depth + 1; vertex < 4; ++vertex) {
            EdgeIR edges;
            for (int anchor = 0; anchor <= depth; ++anchor)
                edges[anchor] = matrix[anchor * 4 + vertex] == '1';
            VertexSetIR set(edges, EdgeRestrictIR{}, depth, mode);
            set.id = id++;
            out.set_ops[depth].push_back(set);
        }
    return out;
}
} // namespace
int main() {
    for (auto mode : {VertexInduced, EdgeInduced, EdgeInducedIEP}) {
        auto star = logical("0111100010001000", mode);
        const auto domains = analyze_domains(star);
        const auto representations = select_representations(star, domains);
        require(domains.regions.size() == 2, "Star should share center universe at two entry depths");
        for (const auto &[id, domain] : domains.sets)
            require(domain.neighborhood_anchors == std::vector<int>{0}, "Lost common-neighbor proof");
        for (const auto &region : domains.regions)
            require(region.anchor_depth == 0 && region.remaining_vertices.size() >= 2,
                    "Invalid star region");
        rejects([&] {
            auto bad = domains;
            bad.sets.begin()->second.neighborhood_anchors.push_back(1);
            verify_representations(star, bad, representations);
        });
        rejects([&] {
            auto bad = domains;
            bad.regions.front().anchor_depth = 1;
            verify_representations(star, bad, representations);
        });
        rejects([&] {
            auto bad = representations;
            bad.sets.begin()->second = {SetStorage::UniverseBitmap, 0};
            verify_representations(star, domains, bad);
        });
        rejects([&] {
            auto bad = representations;
            bad.sets.begin()->second.universe = 0;
            verify_representations(star, domains, bad);
        });
        const auto path = analyze_domains(logical("0100101001010010", mode));
        require(path.regions.empty(), "Path has no common matched neighbor for its suffix");
        const auto clique = analyze_domains(logical("0111101111011110", mode));
        require(clique.regions.size() == 3, "Keep all legal clique anchors for future cost selection");
        auto restricted = star;
        for (auto &level : restricted.set_ops)
            for (auto &set : level) {
                const int id = set.id;
                set = VertexSetIR(EdgeIR(1), EdgeRestrictIR(7), set.loop_depth(), mode);
                set.id = id;
            }
        require(dump_domains(analyze_domains(restricted), representations) ==
                    dump_domains(domains, representations),
                "Ordering restrictions changed containment");
    }
    std::cout
        << "Validated shared-universe proofs, negative cases, and array-only representation policy\n";
}
