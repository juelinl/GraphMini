#include "compiler/execution_ir.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace minigraph {
namespace {
bool contains(const std::vector<int> &values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
std::optional<BitmapProjectionPair> projected_partition(const PlanIR &plan, const ExecutionIR &ir,
                                                        const BitmapRegionExecution &region) {
    if (!plan.context.config.bitmapDirect || !region.full_region || region.full_live_ins.size() != 2)
        return {};
    // Algebraic rule, independent of pattern identity:
    // ((U intersect/difference N(local)) below local) intersect N(external).
    // The pair shares one bounded external projection and partitions it by the
    // local row. Require U or a provably redundant bound, not an arbitrary prefix.
    BitmapProjectionPair pair{-1, -1, -1, region.entry_depth, {}};
    for (int id : region.full_live_ins) {
        const auto &op = ir.sets.at(id);
        if (op.depth != region.entry_depth || op.result != SetResult::Materialize ||
            op.input.source != SetSource::Prefix || op.steps.size() != 1) return {};
        const auto &step = op.steps.front();
        if (step.opcode != SetOpcode::Intersect || !step.rhs ||
            step.rhs->source != SetSource::GraphAdjacency || step.rhs->id != region.entry_depth ||
            step.upper_bound) return {};
        const auto &base = ir.sets.at(op.input.id);
        if (base.depth <= region.build_depth || base.depth >= region.entry_depth ||
            base.result != SetResult::Materialize || base.steps.size() != 1) return {};
        bool universe_input = base.input.source == SetSource::GraphAdjacency &&
                              base.input.id == region.anchor_depth;
        if (base.input.source == SetSource::Prefix) {
            const auto &root = ir.sets.at(base.input.id);
            // A bound-only universe is also valid if local was selected from
            // that very set: x < local implies x satisfies the older bound.
            universe_input = root.input.source == SetSource::GraphAdjacency &&
                root.input.id == region.anchor_depth && root.steps.size() == 1 &&
                root.steps.front().opcode == SetOpcode::Bound &&
                plan.logical.iter_set.at(base.depth - 1).id == root.id;
        }
        if (!universe_input) return {};
        const auto &b = base.steps.front();
        if (!b.rhs || b.rhs->source != SetSource::GraphAdjacency || b.rhs->id != base.depth ||
            !b.upper_bound) return {};
        const auto &bound = *b.upper_bound;
        const auto bound_depth = bound.adjacency ? bound.adjacency->id : bound.depth;
        if ((bound.adjacency && bound.adjacency->source != SetSource::GraphAdjacency) ||
            bound_depth != base.depth || (pair.local_depth >= 0 && pair.local_depth != base.depth)) return {};
        if (plan.logical.adjacency[region.anchor_depth * plan.logical.p_size + base.depth] != '1') return {};
        pair.local_depth = base.depth;
        if (b.opcode == SetOpcode::Intersect) pair.positive = id;
        else if (b.opcode == SetOpcode::DifferenceExcludingOwner) pair.negative = id;
        else return {};
        pair.fallback_sets.push_back(base.id);
        pair.fallback_sets.push_back(id);
    }
    if (pair.positive < 0 || pair.negative < 0) return {};
    // Do not elide a prefix needed by array iteration before the bitmap suffix,
    // or by an unrelated definition. Later consumers belong to the full region;
    // the original expressions remain available in its array fallback.
    for (int depth = 0; depth < region.entry_depth; ++depth)
        if (contains(pair.fallback_sets, plan.logical.iter_set.at(depth).id)) return {};
    for (const auto &entry : ir.sets) {
        const int id = entry.first;
        const auto &op = entry.second;
        auto safe = [&](const SetReference &ref) {
            return ref.source != SetSource::Prefix || !contains(pair.fallback_sets, ref.id) ||
                   contains(pair.fallback_sets, id) || op.depth > region.entry_depth;
        };
        if (!safe(op.input)) return {};
        for (const auto &step : op.steps)
            if (step.rhs && !safe(*step.rhs)) return {};
    }
    return pair;
}
void extend_full_region(const PlanIR &plan, const ExecutionIR &ir, BitmapRegionExecution &region) {
    // Conservative all-or-nothing extension: prefix operands, local row/bound,
    // one lowered step. No ad hoc global-ID conversion inside the region.
    // SSA definitions keep each active ancestor cursor's backing slot intact.
    std::vector<int> slots, inputs;
    auto use = [&](int id) {
        if (!contains(ir.domains.sets.at(id).neighborhood_anchors, region.anchor_depth)) return false;
        if (!contains(slots, id)) slots.push_back(id);
        if (ir.sets.at(id).depth <= region.entry_depth && !contains(inputs, id)) inputs.push_back(id);
        return true;
    };
    for (int depth = region.entry_depth + 1; depth <= plan.logical.p_size - 2; ++depth) {
        if (!use(plan.logical.iter_set.at(depth - 1).id)) return;
        for (const auto &logical : plan.logical.set_ops.at(depth)) {
            const auto &op = ir.sets.at(logical.id);
            if (op.input.source != SetSource::Prefix || op.steps.size() != 1 || !use(op.input.id)) return;
            const auto &step = op.steps.front();
            auto local = [&](const VertexReference &v) {
                return v.adjacency ? v.adjacency->source == SetSource::GraphAdjacency && v.adjacency->id == depth
                                   : v.depth == depth;
            };
            if (step.opcode == SetOpcode::Bound || step.opcode == SetOpcode::Remove) {
                if (!step.vertex || !local(*step.vertex)) return;
            } else if (step.opcode == SetOpcode::Intersect || step.opcode == SetOpcode::DifferenceExcludingOwner) {
                if (!step.rhs || step.rhs->source != SetSource::GraphAdjacency || step.rhs->id != depth ||
                    (step.upper_bound && !local(*step.upper_bound))) return;
            } else return;
            if (op.result != SetResult::Count && !use(op.id)) return;
        }
    }
    region.full_region = true;
    region.full_sets = std::move(slots);
    region.full_live_ins = std::move(inputs);
}
std::optional<BitmapRegionExecution> candidate(const PlanIR &plan, const ExecutionIR &ir,
                                               std::string &reason) {
    const auto &config = plan.context.config;
    if (!config.bitmap) {
        reason = "disabled";
        return {};
    }
    if (config.adjMatType == EdgeInducedIEP || config.pruningType != PruningType::None ||
        config.runnerType != RunnerType::Benchmark ||
        plan.logical.p_size < 4) {
        reason = "requires non-IEP matching, no MiniGraph, benchmark, and at least four vertices";
        return {};
    }
    const int latest_entry = plan.logical.p_size - 4;
    const int conversion = plan.logical.p_size - 3;
    // Prefer the earliest legal execution scope. Immutable construction has
    // its own dependency scope and need not wait for the bitmap-only suffix.
    // Profitability remains separate; the array backend is still the default.
    for (const auto &region : ir.domains.regions) {
        const int entry = region.entry_depth;
        if (entry > latest_entry)
            continue;
        if (!contains(ir.domains.sets.at(plan.logical.iter_set.at(conversion).id).neighborhood_anchors,
                      region.anchor_depth))
            continue;
        BitmapRegionExecution out{entry, conversion, region.anchor_depth, region.anchor_depth, {}, {}};
        bool valid = true;
        for (const auto &logical : plan.logical.set_ops.at(conversion + 1)) {
            const auto &op = ir.sets.at(logical.id);
            if (op.result != SetResult::Count || op.input.source != SetSource::Prefix ||
                ir.sets.at(op.input.id).depth > conversion || op.steps.size() != 1) {
                valid = false;
                break;
            }
            const auto &step = op.steps.front();
            const bool unary = step.opcode == SetOpcode::Bound || step.opcode == SetOpcode::Remove;
            const bool local_unary = unary && step.vertex &&
                (step.vertex->adjacency ? step.vertex->adjacency->source == SetSource::GraphAdjacency &&
                 step.vertex->adjacency->id == conversion + 1 : step.vertex->depth == conversion + 1);
            const bool local_bound = !step.upper_bound ||
                (step.upper_bound->adjacency &&
                 step.upper_bound->adjacency->source == SetSource::GraphAdjacency &&
                 step.upper_bound->adjacency->id == conversion + 1) ||
                (!step.upper_bound->adjacency && step.upper_bound->depth == conversion + 1);
            if ((!local_unary && ((step.opcode != SetOpcode::Intersect &&
                 step.opcode != SetOpcode::DifferenceExcludingOwner) ||
                !step.rhs || step.rhs->source != SetSource::GraphAdjacency ||
                step.rhs->id != conversion + 1)) || !local_bound ||
                !contains(ir.domains.sets.at(op.input.id).neighborhood_anchors, region.anchor_depth)) {
                valid = false;
                break;
            }
            if (!contains(out.live_ins, op.input.id))
                out.live_ins.push_back(op.input.id);
            out.count_ops.push_back(op.id);
        }
        if (valid && !out.count_ops.empty()) {
            out.iterator_set = plan.logical.iter_set.at(conversion).id;
            if (!contains(out.live_ins, out.iterator_set))
                out.live_ins.push_back(out.iterator_set);
            extend_full_region(plan, ir, out);
            out.projection_pair = projected_partition(plan, ir, out);
            const bool unary_terminal = std::any_of(out.count_ops.begin(), out.count_ops.end(), [&](int id) {
                const auto opcode = ir.sets.at(id).steps.front().opcode;
                return opcode == SetOpcode::Bound || opcode == SetOpcode::Remove;
            });
            if ((config.parType != ParallelType::OpenMP || plan.query.mode == EdgeInduced || unary_terminal) && !out.full_region)
                continue; // Task capture currently requires a fully local region.
            reason = "terminal counts reuse one neighborhood BitGraph across at least two matching loops";
            lower_bitmap_bindings(ir, out);
            return out;
        }
    }
    reason = "no supported shared-universe terminal region";
    return {};
}
} // namespace
void lower_bitmap_bindings(const ExecutionIR &ir, BitmapRegionExecution &region) {
    const auto &sets = region.full_region ? region.full_sets : region.live_ins;
    const auto &inputs = region.full_region ? region.full_live_ins : region.live_ins;
    region.slots.clear();
    region.bindings.clear();
    for (size_t slot = 0; slot < sets.size(); ++slot)
        region.slots.emplace(sets[slot], static_cast<int>(slot));
    for (int id : inputs)
        region.bindings.push_back({id, region.slots.at(id), std::max(region.entry_depth, ir.sets.at(id).depth)});
    verify_bitmap_bindings(ir, region);
}

void verify_bitmap_bindings(const ExecutionIR &ir, const BitmapRegionExecution &region) {
    if (region.build_depth < 0 || region.build_depth > region.entry_depth ||
        region.entry_depth > region.conversion_depth)
        throw std::logic_error("Invalid bitmap binding scope");
    const auto &sets = region.full_region ? region.full_sets : region.live_ins;
    const auto &inputs = region.full_region ? region.full_live_ins : region.live_ins;
    if (sets.empty() || inputs.empty() || region.slots.size() != sets.size() || region.bindings.size() != inputs.size())
        throw std::logic_error("Incomplete bitmap slots or bindings");
    for (size_t slot = 0; slot < sets.size(); ++slot) {
        const auto found = region.slots.find(sets[slot]);
        if (found == region.slots.end() || found->second != static_cast<int>(slot) ||
            ir.sets.at(sets[slot]).result == SetResult::Count)
            throw std::logic_error("Invalid bitmap slot identity");
    }
    std::set<int> bound;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto &binding = region.bindings[i];
        if (binding.set_id != inputs[i] || !bound.insert(binding.set_id).second ||
            binding.slot != region.slots.at(binding.set_id))
            throw std::logic_error("Invalid bitmap live-in binding");
        const int definition = ir.sets.at(binding.set_id).depth;
        // No delayed or hoisted address capture: once candidate state exists,
        // bind exactly at the definition (or entry for an older definition).
        if (binding.depth < region.entry_depth || binding.depth < definition ||
            (binding.depth != region.entry_depth && binding.depth != definition) ||
            binding.depth > region.conversion_depth || (region.full_region && binding.depth != region.entry_depth))
            throw std::logic_error("Invalid bitmap binding lifetime");
    }
}

void lower_bitmap_region(const PlanIR &plan, ExecutionIR &ir) {
    ir.bitmap_region = candidate(plan, ir, ir.bitmap_reason);
}
void verify_bitmap_region(const PlanIR &plan, const ExecutionIR &ir) {
    std::string reason;
    const auto expected = candidate(plan, ir, reason);
    if (reason != ir.bitmap_reason || expected.has_value() != ir.bitmap_region.has_value())
        throw std::logic_error("Invalid bitmap region selection");
    if (!expected)
        return;
    const auto &actual = *ir.bitmap_region;
    if (actual.entry_depth != expected->entry_depth || actual.anchor_depth != expected->anchor_depth ||
        actual.build_depth != expected->build_depth ||
        actual.conversion_depth != expected->conversion_depth || actual.live_ins != expected->live_ins ||
        actual.count_ops != expected->count_ops || actual.iterator_set != expected->iterator_set ||
        actual.full_region != expected->full_region || actual.full_sets != expected->full_sets ||
        actual.full_live_ins != expected->full_live_ins ||
        actual.projection_pair.has_value() != expected->projection_pair.has_value() ||
        (actual.projection_pair && !(*actual.projection_pair == *expected->projection_pair)))
        throw std::logic_error("Invalid bitmap scope, identity, rows, or live-ins");
    verify_bitmap_bindings(ir, actual);
}
} // namespace minigraph
