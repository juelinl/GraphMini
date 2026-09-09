#include "compiler/codegen/cpp.h"
#include "compiler/codegen/names.h"
#include <algorithm>
#include <fmt/format.h>
#include <string>

namespace minigraph {
// Full regions have already been verified to contain single-step operations in
// one universe. Backends supply state access and empty-result control flow;
// task boundaries, iteration, reduction and progress remain at the call sites.
std::string CppCodegen::emit_bitmap_ops(const PlanIR &plan, int depth, const BitmapEmission &target) {
    const auto &sets = execution_.bitmap_region->full_sets;
    const auto slot = [&](int id) { return std::find(sets.begin(), sets.end(), id) - sets.begin(); };
    const auto vertex = codegen_names::bit_index(depth);
    std::string out;
    for (const auto &logical : plan.logical.set_ops.at(depth)) {
        const auto &op = execution_.sets.at(logical.id);
        const auto &step = op.steps.front();
        const bool subtract = step.opcode == SetOpcode::DifferenceExcludingOwner;
        const bool bound = step.opcode == SetOpcode::Bound;
        const bool remove = step.opcode == SetOpcode::Remove;
        const bool bounded = bound || step.upper_bound.has_value();
        if (op.result == SetResult::Count) {
            out += fmt::format("counter += {}count_local<bitmap_words>({}, {}, {}, {}, {}, {});\n", target.state_access,
                               slot(op.input.id), vertex, subtract, bounded, bound || remove, remove);
            if (bitmap_diagnostics_)
                out += "bitmap_counters[3].fetch_add(1, std::memory_order_relaxed);\n";
        } else {
            if (op.guard_empty || op.result == SetResult::MaterializeThenCount)
                out += fmt::format("const auto bn{} = ", op.id);
            out += fmt::format("{}materialize_local<bitmap_words>({}, {}, {}, {}, {}, {}, {});\n", target.state_access,
                               slot(op.id), slot(op.input.id), vertex, subtract, bounded, bound || remove, remove);
            if (op.guard_empty)
                out += fmt::format("if (!bn{}) {}\n", op.id, target.empty_action);
            if (op.result == SetResult::MaterializeThenCount)
                out += fmt::format("counter += bn{};\n", op.id);
        }
    }
    return out;
}
} // namespace minigraph
