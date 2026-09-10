#pragma once
#include "bitmap_count_region.h"
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <functional>
#include <cstdlib>
#include <string>

namespace minigraph {
// Policy selection is read once when a generated module loads.
// Separate processes can compare policies without recompiling the query.
struct BitmapTaskPolicy {
    size_t grain{16};
    size_t levels{std::numeric_limits<size_t>::max()};
    bool skip_empty{false};
    bool copy_inputs{false}; // Borrow large inputs; keep outputs private until the synchronous join.
    static BitmapTaskPolicy from_environment() {
        const char *value = std::getenv("GRAPHMINI_BITMAP_TASK_POLICY");
        const std::string name = value ? value : "baseline";
        if (name == "baseline") return {};
        if (name == "empty16") return {16, std::numeric_limits<size_t>::max(), true};
        if (name == "grain64") return {64, std::numeric_limits<size_t>::max(), true};
        if (name == "grain64-copy") return {64, std::numeric_limits<size_t>::max(), true, true};
        if (name == "grain64-borrow") return {64, std::numeric_limits<size_t>::max(), true, false};
        if (name == "grain128") return {128, std::numeric_limits<size_t>::max(), true};
        if (name == "shallow64") return {64, 1, true};
        throw std::invalid_argument("Unknown GRAPHMINI_BITMAP_TASK_POLICY");
    }
};
} // namespace minigraph

namespace minigraph {
// Own only task-local copies; otherwise borrow an immutable named input until
// the synchronous join. Never keep a view into this wrapper after its lifetime.
class BitmapTaskInput {
    std::optional<Bitmap> owned_;
    const Bitmap *input_;
  public:
    BitmapTaskInput(const Bitmap &input, bool task, const BitmapTaskPolicy &policy)
        : input_(&input) {
        if (task && (policy.copy_inputs || input.words().is_inline()))
            owned_.emplace(input);
    }
    BitmapTaskInput(Bitmap &&, bool, const BitmapTaskPolicy &) = delete;
    const Bitmap &get() const & { return owned_ ? *owned_ : *input_; }
    const Bitmap &get() const && = delete;
};

// The generated body owns named scratch values and the cursor. This helper
// only splits ranges and joins reductions; it does not interpret set operations.
template<class Function>
uint64_t bitmap_for_each(const Bitmap &input, bool parallel, const Function &range_body,
                         const BitmapTaskPolicy &policy = {}, size_t level = 0) {
    const auto bits = input.universe().size();
    const auto candidates = input.count();
    // An empty matching loop has no body work. Avoid constructing private
    // scratch for it, particularly when the universe uses pooled buffers.
    if (!candidates) return 0;
    if (!parallel || level >= policy.levels || candidates < 2)
        return range_body(0, bits, false);
    return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, bits, policy.grain), uint64_t{0},
        [&](const tbb::blocked_range<size_t> &range, uint64_t count) {
            if (policy.skip_empty && !input.local_cursor(range.begin(), range.end()).valid()) return count;
            return count + range_body(range.begin(), range.end(), true);
        }, std::plus<uint64_t>{});
}

// No mutable state is stored in a TBB body: each invocation has its own slots
// and reduction accumulator. Parent state stays read-only until the join.
template<class Function>
uint64_t bitmap_for_each(BitmapCountRegion &state, size_t input, bool parallel,
                         const Function &function, const BitmapTaskPolicy &policy = {}, size_t level = 0) {
    auto serial = [&](BitmapCountRegion &local, size_t begin, size_t end) {
        uint64_t count = 0;
        for (auto cursor = local.local_cursor(input, begin, end); cursor.valid(); cursor.advance())
            count += function(local, cursor.position());
        return count;
    };
    if (!parallel || level >= policy.levels || state.input_size(input) < 2)
        return serial(state, 0, state.universe_size());
    return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, state.universe_size(), policy.grain), uint64_t{0},
        [&](const tbb::blocked_range<size_t> &range, uint64_t count) {
            if (policy.skip_empty && !state.local_cursor(input, range.begin(), range.end()).valid())
                return count;
            auto local = policy.copy_inputs ? state.fork() : state.fork_borrowed();
            return count + serial(local, range.begin(), range.end());
        }, std::plus<uint64_t>{});
}
} // namespace minigraph
