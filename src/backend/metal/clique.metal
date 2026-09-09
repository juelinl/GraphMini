#include <metal_stdlib>
using namespace metal;
constant uint query_size [[function_constant(0)]];
struct Region { ulong offset; uint degree; uint stride; };
struct Task { uint region; uint local_id; };
struct Parameters { uint tasks; uint scratch_stride; };
struct Count { ulong matches; ulong overflow; };

// Fixed-depth template expansion keeps one/two-word candidate masks as scalar
// values rather than dynamically indexing a DFS stack in private memory.
template <uint Left>
inline Count small_suffix(device const ulong *matrix, ulong low, ulong high, uint stride);
template <>
inline Count small_suffix<1>(device const ulong *matrix, ulong low, ulong high, uint stride) {
    return {ulong(popcount(low))+ulong(popcount(high)),0};
}
template <uint Left>
inline Count small_suffix(device const ulong *matrix, ulong low, ulong high, uint stride) {
    Count result{0,0};
    while (low || high) {
        uint position;
        if (high) { position = 63u-uint(clz(high)); high &= ~(1ul << position); position += 64; }
        else { position = 63u-uint(clz(low)); low &= ~(1ul << position); }
        const ulong next_low = low & matrix[ulong(position)*stride];
        const ulong next_high = stride == 2 ? high & matrix[ulong(position)*stride+1] : 0;
        const Count child = small_suffix<Left-1>(matrix,next_low,next_high,stride);
        result.overflow |= child.overflow | ulong(child.matches > ~0ul-result.matches);
        result.matches += child.matches;
    }
    return result;
}
inline Count small_root(device const ulong *matrix, uint stride, uint first) {
    const ulong low_mask = first >= 64 ? ~0ul : first == 0 ? 0ul : (1ul << first)-1;
    const ulong high_mask = first <= 64 ? 0ul : (1ul << (first-64))-1;
    const ulong low = matrix[ulong(first)*stride] & low_mask;
    const ulong high = stride == 2 ? matrix[ulong(first)*stride+1] & high_mask : 0;
    switch (query_size) {
        case 3: return small_suffix<1>(matrix,low,high,stride);
        case 4: return small_suffix<2>(matrix,low,high,stride);
        case 5: return small_suffix<3>(matrix,low,high,stride);
        case 6: return small_suffix<4>(matrix,low,high,stride);
        case 7: return small_suffix<5>(matrix,low,high,stride);
        default: return small_suffix<6>(matrix,low,high,stride);
    }
}

template <typename Stack>
inline Count traverse(device const ulong *matrix, Stack stack, uint stride, uint first) {
    for (uint w = 0; w < stride; ++w) {
        const uint start = w*64;
        const uint bits = first > start ? min(64u, first-start) : 0u;
        const ulong mask = bits == 64 ? ~0ul : bits == 0 ? 0ul : (1ul << bits)-1;
        stack[w] = matrix[ulong(first)*stride+w] & mask;
    }
    ulong count = 0;
    bool overflow = false;
    int depth = 0;
    while (depth >= 0) {
        auto current = stack + uint(depth)*stride;
        if (uint(depth) == query_size-3) {
            for (uint w = 0; w < stride; ++w) {
                const ulong value = ulong(popcount(current[w]));
                if (value > ~0ul-count) overflow = true;
                count += value;
            }
            --depth;
            continue;
        }
        int word = int(stride)-1;
        while (word >= 0 && current[word] == 0) --word;
        if (word < 0) { --depth; continue; }
        const uint bit = 63u-uint(clz(current[word]));
        const uint local_id = uint(word)*64+bit;
        current[word] &= ~(1ul << bit);
        auto child = current+stride;
        for (uint w = 0; w < stride; ++w)
            child[w] = current[w] & matrix[ulong(local_id)*stride+w];
        ++depth;
    }
    return {count, ulong(overflow)};
}
// One independent edge-prefix DFS per GPU thread. Small universes use private
// state; arbitrary larger universes use a bounded device scratch allocation.
// All tasks of a region share immutable rows; descendants strictly decrease.
kernel void clique_count(device const ulong *rows [[buffer(0)]],
                         device const Region *regions [[buffer(1)]],
                         device const Task *tasks [[buffer(2)]],
                         device ulong *scratch [[buffer(3)]],
                         device Count *counts [[buffer(4)]],
                         constant Parameters &params [[buffer(5)]],
                         uint tid [[thread_position_in_grid]]) {
    if (tid >= params.tasks) return;
    const Task task = tasks[tid];
    const Region region = regions[task.region];
    device const ulong *matrix = rows+region.offset;
    if (region.stride <= 2) {
        counts[tid] = small_root(matrix,region.stride,task.local_id);
    } else {
        device ulong *state = scratch+ulong(tid)*params.scratch_stride*(query_size-2);
        counts[tid] = traverse(matrix,state,region.stride,task.local_id);
    }
}
