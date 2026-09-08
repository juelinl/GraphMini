#include "compiler/execution_ir.h"
#include <algorithm>
#include <stdexcept>

namespace minigraph {
namespace {
bool contains(const std::vector<int> &values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
std::optional<BitmapRegionExecution> candidate(const PlanIR &plan, const ExecutionIR &ir,
                                             std::string &reason) {
    const auto &config = plan.context.config;
    if (!config.bitmap) {
        reason = "disabled";
        return {};
    }
    if (plan.query.mode != VertexInduced || config.pruningType != PruningType::None ||
        config.parType != ParallelType::OpenMP || config.runnerType != RunnerType::Benchmark ||
        plan.logical.p_size < 4) {
        reason = "requires vertex-induced, no MiniGraph, OpenMP, benchmark, and at least four vertices";
        return {};
    }
    const int entry = plan.logical.p_size - 3;
    const int row_set = plan.logical.iter_set.at(entry).id;
    for (const auto &region : ir.domains.regions) {
        if (region.entry_depth != entry ||
            !contains(ir.domains.sets.at(row_set).neighborhood_anchors, region.anchor_depth))
            continue;
        BitmapRegionExecution out{entry, region.anchor_depth, row_set, {}, {}};
        bool valid = true;
        for (const auto &logical : plan.logical.set_ops.at(entry + 1)) {
            const auto &op = ir.sets.at(logical.id);
            if (op.result != SetResult::Count || op.input.source != SetSource::Prefix ||
                ir.sets.at(op.input.id).depth > entry || op.steps.size() != 1) {
                valid = false;
                break;
            }
            const auto &step = op.steps.front();
            if ((step.opcode != SetOpcode::Intersect &&
                 step.opcode != SetOpcode::DifferenceExcludingOwner) || !step.rhs ||
                step.rhs->source != SetSource::GraphAdjacency || step.rhs->id != entry + 1 ||
                !contains(ir.domains.sets.at(op.input.id).neighborhood_anchors, region.anchor_depth)) {
                valid = false;
                break;
            }
            if (!contains(out.live_ins, op.input.id))
                out.live_ins.push_back(op.input.id);
            out.count_ops.push_back(op.id);
        }
        if (valid && !out.count_ops.empty()) {
            reason = "terminal counts reuse one neighborhood BitGraph across the final matching loop";
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
        actual.row_set != expected->row_set || actual.live_ins != expected->live_ins ||
        actual.count_ops != expected->count_ops)
        throw std::logic_error("Invalid bitmap scope, identity, rows, or live-ins");
}
} // namespace minigraph
