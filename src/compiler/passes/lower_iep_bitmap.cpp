#include "compiler/execution_ir.h"

namespace minigraph {
std::optional<IEPBitmapExecution> plan_iep_bitmap(const PlanIR &, const ExecutionIR &) {
    // IEP uses arrays: per-evaluation bitmap conversion did not give a reliable win.
    return {};
}
} // namespace minigraph
