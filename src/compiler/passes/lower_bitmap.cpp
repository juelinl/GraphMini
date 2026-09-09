#include "compiler/execution_ir.h"
#include <algorithm>
#include <stdexcept>

namespace minigraph {
namespace {
bool contains(const std::vector<int> &values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
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
    // Prefer the earliest legal scope in this forced experiment: building at
    // a fixed late depth needlessly repeats identical anchor-universe rows.
    // Profitability remains separate; the array backend is still the default.
    for (const auto &region : ir.domains.regions) {
        const int entry = region.entry_depth;
        if (entry > latest_entry)
            continue;
        if (!contains(ir.domains.sets.at(plan.logical.iter_set.at(conversion).id).neighborhood_anchors,
                      region.anchor_depth))
            continue;
        BitmapRegionExecution out{entry, conversion, region.anchor_depth, {}, {}};
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
            const bool unary_terminal = std::any_of(out.count_ops.begin(), out.count_ops.end(), [&](int id) {
                const auto opcode = ir.sets.at(id).steps.front().opcode;
                return opcode == SetOpcode::Bound || opcode == SetOpcode::Remove;
            });
            if ((config.parType != ParallelType::OpenMP || plan.query.mode == EdgeInduced || unary_terminal) && !out.full_region)
                continue; // Task capture currently requires a fully local region.
            reason = "terminal counts reuse one neighborhood BitGraph across at least two matching loops";
            return out;
        }
    }
    reason = "no supported shared-universe terminal region";
    return {};
}
} // namespace
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
        actual.conversion_depth != expected->conversion_depth || actual.live_ins != expected->live_ins ||
        actual.count_ops != expected->count_ops || actual.iterator_set != expected->iterator_set ||
        actual.full_region != expected->full_region || actual.full_sets != expected->full_sets ||
        actual.full_live_ins != expected->full_live_ins)
        throw std::logic_error("Invalid bitmap scope, identity, rows, or live-ins");
}
} // namespace minigraph
