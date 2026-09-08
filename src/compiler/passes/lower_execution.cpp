#include "compiler/execution_ir.h"
#include <stdexcept>

namespace minigraph {
namespace {
SetReference prefix(int id) { return {SetSource::Prefix, id}; }
SetReference adjacency(int depth) { return {SetSource::GraphAdjacency, depth}; }
VertexReference selected(int depth) { return {depth, std::nullopt}; }
VertexReference owner(SetReference set) { return {-1, set}; }
SetStep bound(VertexReference vertex) { return {SetOpcode::Bound, {}, vertex, {}}; }
SetStep remove(VertexReference vertex) { return {SetOpcode::Remove, {}, vertex, {}}; }
SetStep binary(SetOpcode opcode, SetReference rhs, bool restricted) {
    return {opcode, rhs, {}, restricted ? std::optional<VertexReference>(owner(rhs)) : std::nullopt};
}

SetExecution lower_set(const PlanIR &plan, const VertexSetIR &op) {
    const int depth = op.loop_depth();
    SetExecution out{op.id, depth, adjacency(depth), {}};
    const auto parent = plan.get_parent_vset(op);
    const auto mg = plan.context.config.pruningType == PruningType::None ? std::optional<MiniGraphIR>{}
                                                                 : plan.get_parent_mg(op);
    const bool restricted = op.is_restricted(depth);
    const bool vertex_induced = plan.query.mode == AdjMatType::VertexInduced;
    // Preserve the current terminal-cardinality policy independently of set semantics.
    const bool terminal = depth == plan.logical.p_size - 2;
    if (mg) {
        if (!parent)
            throw std::logic_error("MiniGraph operation has no prefix");
        const SetReference rhs{SetSource::MiniGraphAdjacency, mg->id};
        if (mg->computed(plan.logical.iter_set.at(depth - 1), op)) {
            if (terminal)
                throw std::logic_error("Terminal MiniGraph alias");
            out.input = rhs;
            if (restricted)
                out.steps.push_back(bound(selected(depth)));
            out.rules.push_back("minigraph-computed: reuse adjacency directly");
        } else {
            if (!op.is_edge(depth) && !vertex_induced)
                throw std::logic_error("Edge-induced MiniGraph difference");
            out.input = prefix(parent->id);
            out.steps.push_back(
                binary(op.is_edge(depth) ? SetOpcode::Intersect : SetOpcode::DifferenceExcludingOwner,
                       rhs, restricted));
            if (terminal)
                out.result = SetResult::Count;
            out.rules.push_back("minigraph-source: use selected auxiliary adjacency");
        }
        out.guard_empty = !terminal;
    } else if (parent) {
        out.input = prefix(parent->id);
        if (parent->loop_depth() == depth) {
            if (!restricted)
                throw std::logic_error("Same-depth prefix needs a bound");
            out.steps.push_back(bound(selected(depth)));
            out.rules.push_back("same-depth-prefix: restricted prefix view");
        } else {
            if (parent->loop_depth() != depth - 1)
                throw std::logic_error("Nonadjacent prefix dependency");
            if (op.is_edge(depth) || vertex_induced)
                out.steps.push_back(binary(op.is_edge(depth) ? SetOpcode::Intersect
                                                             : SetOpcode::DifferenceExcludingOwner,
                                           adjacency(depth), restricted));
            else
                out.steps.push_back(restricted ? bound(owner(adjacency(depth)))
                                               : remove(owner(adjacency(depth))));
            if (terminal)
                out.result = SetResult::Count;
            out.rules.push_back("prefix-reuse: extend previous-depth prefix");
        }
    } else {
        if (op.edge_num() != 1 || !op.is_edge(depth))
            throw std::logic_error("Root set must start from its single adjacency");
        if (restricted)
            out.steps.push_back(bound(selected(depth)));
        for (int prior = 0; prior < depth; ++prior) {
            if (vertex_induced)
                out.steps.push_back(binary(SetOpcode::DifferenceExcludingOwner, adjacency(prior),
                                           op.is_restricted(prior)));
            else
                out.steps.push_back(op.is_restricted(prior) ? bound(owner(adjacency(prior)))
                                                            : remove(owner(adjacency(prior))));
        }
        out.guard_empty = true;
        if (terminal)
            out.result = SetResult::MaterializeThenCount;
        out.rules.push_back("adjacency-root: apply earlier-vertex constraints in order");
    }
    if (out.result == SetResult::Count)
        out.rules.push_back("terminal-cardinality: legacy final-depth count-only policy");
    return out;
}
} // namespace

ExecutionIR lower_execution(const PlanIR &plan) {
    ExecutionIR result;
    result.domains = analyze_domains(plan.logical);
    result.representations = select_representations(plan.logical, result.domains);
    for (const auto &level : plan.logical.set_ops)
        for (const auto &op : level)
            if (!result.sets.emplace(op.id, lower_set(plan, op)).second)
                throw std::logic_error("Duplicate execution set ID");
    for (size_t group = 0; plan.counting.iep_num > 1 && group < plan.counting.iep_groups.size(); ++group) {
        IEPTerm term{plan.counting.iep_vals.at(group), {}};
        for (const auto &factor : plan.counting.iep_groups.at(group)) {
            std::vector<int> ids;
            for (int index : factor)
                ids.push_back(plan.counting.iep_set.at(index).id);
            // Preserve the established pair simplification, including logical
            // aliases.
            if (factor.size() == 2 && plan.counting.iep_set.at(factor[0]) == plan.counting.iep_set.at(factor[1]))
                ids.resize(1);
            term.factors.push_back(std::move(ids));
        }
        result.iep.push_back(std::move(term));
    }
    lower_minigraphs(plan, result);
    lower_loops(plan, result);
    verify_execution(result, plan);
    return result;
}
} // namespace minigraph
