#pragma once
#include "bitmap_count_region.h"
#include "bit_ops/decode.h"
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <functional>
#include <cstdlib>
#include <string>
#include <memory>

namespace minigraph {
enum class BitmapIteration { Positions, DecodedScalar, DecodedAVX2 };
// Policy selection is read once when a generated module loads.
// Separate processes can compare policies without recompiling the query.
struct BitmapTaskPolicy {
    size_t grain{16};
    size_t levels{std::numeric_limits<size_t>::max()};
    bool skip_empty{false};
    bool copy_inputs{false}; // Borrow large inputs; keep outputs private until the synchronous join.
    BitmapIteration iteration{BitmapIteration::Positions}; // Opt-in decode-first experiment.
    static BitmapTaskPolicy from_environment() {
        const char *value = std::getenv("GRAPHMINI_BITMAP_TASK_POLICY");
        const std::string name = value ? value : "baseline";
        BitmapTaskPolicy policy;
        if (name == "baseline") {}
        else if (name == "empty16") policy = {16, std::numeric_limits<size_t>::max(), true};
        else if (name == "grain64" || name == "grain64-borrow") policy = {64, std::numeric_limits<size_t>::max(), true};
        else if (name == "grain64-copy") policy = {64, std::numeric_limits<size_t>::max(), true, true};
        else if (name == "grain128") policy = {128, std::numeric_limits<size_t>::max(), true};
        else if (name == "shallow64") policy = {64, 1, true};
        else throw std::invalid_argument("Unknown GRAPHMINI_BITMAP_TASK_POLICY");
        const char *iteration = std::getenv("GRAPHMINI_BITMAP_ITERATION");
        const std::string mode = iteration ? iteration : "positions";
        if (mode == "decoded-scalar") policy.iteration = BitmapIteration::DecodedScalar;
        else if (mode == "decoded-avx2") policy.iteration = BitmapIteration::DecodedAVX2;
        else if (mode != "positions") throw std::invalid_argument("Unknown GRAPHMINI_BITMAP_ITERATION");
        return policy;
    }
};
} // namespace minigraph

namespace minigraph {
// A non-owning slice of sorted LOCAL indices. Its owner spans the synchronous
// reduction; copies of this cursor never allocate or copy the index array.
class BitmapIndexCursor {
    const uint32_t *indices_;
    size_t current_, end_;
  public:
    BitmapIndexCursor(const uint32_t *indices, size_t begin, size_t end)
        : indices_(indices), current_(begin), end_(end) {}
    bool valid() const { return current_ < end_; }
    uint32_t position() const {
        if (!valid()) throw std::out_of_range("Exhausted bitmap index cursor");
        return indices_[current_];
    }
    void advance() { if (valid()) ++current_; }
};
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
        return range_body(input.local_cursor(), false);
    if (!policy.grain) throw std::invalid_argument("Bitmap task grain must be positive");
    if (policy.iteration != BitmapIteration::Positions) {
        // Allocate/decode exactly once per parallel level invocation, not per
        // task. No global-ID conversion. Storage survives until every child joins.
        std::unique_ptr<uint32_t[]> indices(new uint32_t[candidates]);
        const auto written = policy.iteration == BitmapIteration::DecodedAVX2
            ? bit_ops::decode_indices_avx2(input.words().data(), bits, indices.get())
            : bit_ops::decode_indices_scalar(input.words().data(), bits, indices.get());
        if (written != candidates) throw std::logic_error("Bitmap decode cardinality mismatch");
        return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, candidates, policy.grain), uint64_t{0},
            [&](const tbb::blocked_range<size_t> &range, uint64_t count) {
                return count + range_body(BitmapIndexCursor(indices.get(), range.begin(), range.end()), true);
            }, std::plus<uint64_t>{});
    }
    return tbb::parallel_reduce(tbb::blocked_range<size_t>(0, bits, policy.grain), uint64_t{0},
        [&](const tbb::blocked_range<size_t> &range, uint64_t count) {
            if (policy.skip_empty && !input.local_cursor(range.begin(), range.end()).valid()) return count;
            return count + range_body(input.local_cursor(range.begin(), range.end()), true);
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
