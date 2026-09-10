// Naming (D is matching depth; N is an IR set ID):
// sN         : array-backed vertex set with IR set ID N
// bN         : bitmap with IR set ID N (same ID as its array representation)
// vD         : global vertex ID matched at depth D
// vD_idx     : position in the prefix set iterated at depth D
// vD_adj     : adjacency list or bitmap row of vD
// vD_bit_idx : position of vD in the current bitmap universe
// SetLevelD / BitLevelD : array / bitmap task at matching depth D

// bitmap: terminal counts reuse one neighborhood BitGraph across at least two matching loops
#include "plan.h"
#include "backend/benchmark_progress.h"
#include <array>
#include "runtime/nested_policy.h"
#include "backend/bitmap_tasks.h"
#include "backend/bitmap_dispatch.h"
namespace minigraph {
// Borrowed per-query state; all task joins complete before plan returns.
struct QueryContext {
    const Graph *const graph;
    Context &ctx;
    BenchmarkProgress &progress;
    const std::array<size_t, 6> nested_thresholds;
};
static const auto bitmap_task_policy = BitmapTaskPolicy::from_environment();
uint64_t pattern_size() { return 7; }
// Bitmap level definitions: borrowed inputs; invocation-local scratch.
template <size_t bitmap_words>
class BitLevel5 {
    const QueryContext &query;
    const BitGraph &bitgraph;
    const BitmapTaskPolicy &policy;
    const Bitmap &input_b13;
    const Bitmap &input_b14;

  public:
    BitLevel5(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_b13, const Bitmap &input_b14)
        : query(query), bitgraph(bitgraph), policy(policy), input_b13(input_b13), input_b14(input_b14) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_b14, false, *this, policy, 1, 0);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc5, bool parallel_task) const {
        BitmapTaskInput task_b13(input_b13, parallel_task, policy);
        const Bitmap &b13 = task_b13.get();
        BitmapTaskInput task_b14(input_b14, parallel_task, policy);
        const Bitmap &b14 = task_b14.get();
        uint64_t counter = 0;
        for (; bc5.valid(); bc5.advance()) { // bitmap local-index loop
            const auto v5_bit_idx = bc5.position();
            const uint64_t previous_count = counter;
            const auto v5_adj = bitgraph.local_row(v5_bit_idx);
            counter += b13.subtraction_count<bitmap_words>(v5_adj, v5_bit_idx);
            query.progress.add_bitmap_matches(counter - previous_count);
        }
        return counter;
    }
};
template <size_t bitmap_words>
class BitLevel4 {
    const QueryContext &query;
    const BitGraph &bitgraph;
    const BitmapTaskPolicy &policy;
    const Bitmap &input_b10;
    const Bitmap &input_b11;
    const Bitmap &input_b12;

  public:
    BitLevel4(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_b10, const Bitmap &input_b11, const Bitmap &input_b12)
        : query(query), bitgraph(bitgraph), policy(policy), input_b10(input_b10), input_b11(input_b11), input_b12(input_b12) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_b12, true, *this, policy, 0, query.nested_thresholds[4]);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc4, bool parallel_task) const {
        BitmapTaskInput task_b10(input_b10, parallel_task, policy);
        const Bitmap &b10 = task_b10.get();
        BitmapTaskInput task_b11(input_b11, parallel_task, policy);
        const Bitmap &b11 = task_b11.get();
        BitmapTaskInput task_b12(input_b12, parallel_task, policy);
        const Bitmap &b12 = task_b12.get();
        Bitmap b13(bitgraph.universe());
        Bitmap b14(bitgraph.universe());
        uint64_t counter = 0;
        for (; bc4.valid(); bc4.advance()) { // bitmap local-index loop
            const auto v4_bit_idx = bc4.position();
            const auto v4_adj = bitgraph.local_row(v4_bit_idx);
            b13.assign_subtraction<bitmap_words, true>(b10, v4_adj, v4_bit_idx);
            b14.assign_subtraction<bitmap_words, true>(b11, v4_adj, v4_bit_idx);
            counter += BitLevel5<bitmap_words>(query, bitgraph, policy, b13, b14)();
        }
        return counter;
    }
};
// End bitmap level definitions.
class SetLevel4 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s10;
    VertexSet &s11;
    // Iterate Set
    VertexSet &s12;

  public:
    SetLevel4(const QueryContext &_query, VertexSet &_s10, VertexSet &_s11, VertexSet &_s12) : query{_query}, s10{_s10}, s11{_s11}, s12{_s12} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v4_idx = r.begin(); v4_idx < r.end(); v4_idx++) { // loop-4begin
            const IdType v4 = s12[v4_idx];
            VertexSet v4_adj = query.graph->N(v4);
            VertexSet s13 = s10.subtract(v4_adj);
            VertexSet s14 = s11.subtract(v4_adj);
            for (size_t v5_idx = 0; v5_idx < s14.size(); v5_idx++) { // loop-5 begin
                const IdType v5 = s14[v5_idx];
                VertexSet v5_adj = query.graph->N(v5);
                counter += s13.subtract_cnt(v5_adj);
            }
        }
    }
};

class SetLevel3 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s6;
    VertexSet &s7;
    VertexSet &s9;
    // Iterate Set
    VertexSet &s8;

  public:
    SetLevel3(const QueryContext &_query, VertexSet &_s6, VertexSet &_s7, VertexSet &_s9, VertexSet &_s8) : query{_query}, s6{_s6}, s7{_s7}, s9{_s9}, s8{_s8} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v3_idx = r.begin(); v3_idx < r.end(); v3_idx++) { // loop-3begin
            const IdType v3 = s8[v3_idx];
            VertexSet v3_adj = query.graph->N(v3);
            VertexSet s10 = s6.subtract(v3_adj);
            VertexSet s11 = s7.intersect(v3_adj);
            VertexSet s12 = s9.intersect(v3_adj);
            if (s12.size() > query.nested_thresholds[4]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s12.size(), 1), SetLevel4(query, s10, s11, s12), tbb::auto_partitioner());
                continue;
            }
            for (size_t v4_idx = 0; v4_idx < s12.size(); v4_idx++) { // loop-4 begin
                const IdType v4 = s12[v4_idx];
                VertexSet v4_adj = query.graph->N(v4);
                VertexSet s13 = s10.subtract(v4_adj);
                VertexSet s14 = s11.subtract(v4_adj);
                for (size_t v5_idx = 0; v5_idx < s14.size(); v5_idx++) { // loop-5 begin
                    const IdType v5 = s14[v5_idx];
                    VertexSet v5_adj = query.graph->N(v5);
                    counter += s13.subtract_cnt(v5_adj);
                }
            }
        }
    }
};

class SetLevel2 {
  private:
    const QueryContext &query;
    // Adjacent Lists
    VertexSet &v0_adj;
    VertexSet &v1_adj;
    // Parent Intermediates
    VertexSet &s3;
    VertexSet &s5;
    VertexSet &s4;
    // Iterate Set
    VertexSet &s2;

  public:
    SetLevel2(const QueryContext &_query, VertexSet &_v0_adj, VertexSet &_v1_adj, VertexSet &_s3, VertexSet &_s5, VertexSet &_s4, VertexSet &_s2) : query{_query}, v0_adj{_v0_adj}, v1_adj{_v1_adj}, s3{_s3}, s5{_s5}, s4{_s4}, s2{_s2} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v2_idx = r.begin(); v2_idx < r.end(); v2_idx++) { // loop-2begin
            const IdType v2 = s2[v2_idx];
            VertexSet v2_adj = query.graph->N(v2);
            VertexSet s6 = v2_adj.subtract(v0_adj).subtract(v1_adj);
            if (s6.size() == 0)
                continue;
            VertexSet s7 = s3.intersect(v2_adj);
            VertexSet s8 = s5.subtract(v2_adj);
            VertexSet s9 = s4.intersect(v2_adj);
            if (s8.size() > query.nested_thresholds[3]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s8.size(), 1), SetLevel3(query, s6, s7, s9, s8), tbb::auto_partitioner());
                continue;
            }
            for (size_t v3_idx = 0; v3_idx < s8.size(); v3_idx++) { // loop-3 begin
                const IdType v3 = s8[v3_idx];
                VertexSet v3_adj = query.graph->N(v3);
                VertexSet s10 = s6.subtract(v3_adj);
                VertexSet s11 = s7.intersect(v3_adj);
                VertexSet s12 = s9.intersect(v3_adj);
                if (s12.size() > query.nested_thresholds[4]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s12.size(), 1), SetLevel4(query, s10, s11, s12), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v4_idx = 0; v4_idx < s12.size(); v4_idx++) { // loop-4 begin
                    const IdType v4 = s12[v4_idx];
                    VertexSet v4_adj = query.graph->N(v4);
                    VertexSet s13 = s10.subtract(v4_adj);
                    VertexSet s14 = s11.subtract(v4_adj);
                    for (size_t v5_idx = 0; v5_idx < s14.size(); v5_idx++) { // loop-5 begin
                        const IdType v5 = s14[v5_idx];
                        VertexSet v5_adj = query.graph->N(v5);
                        counter += s13.subtract_cnt(v5_adj);
                    }
                }
            }
        }
    }
};

class SetLevel1 {
  private:
    const QueryContext &query;
    // Adjacent Lists
    VertexSet &v0_adj;
    // Parent Intermediates
    VertexSet &s1;
    // Iterate Set
    VertexSet &s0;

  public:
    SetLevel1(const QueryContext &_query, VertexSet &_v0_adj, VertexSet &_s1, VertexSet &_s0) : query{_query}, v0_adj{_v0_adj}, s1{_s1}, s0{_s0} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v1_idx = r.begin(); v1_idx < r.end(); v1_idx++) { // loop-1begin
            const IdType v1 = s0[v1_idx];
            VertexSet v1_adj = query.graph->N(v1);
            VertexSet s2 = v1_adj.subtract(v0_adj);
            if (s2.size() == 0)
                continue;
            VertexSet s3 = s0.subtract(v1_adj);
            VertexSet s4 = s0.intersect(v1_adj, v1_adj.vid());
            VertexSet s5 = s1.intersect(v1_adj);
            if (s2.size() > query.nested_thresholds[2]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s2.size(), 1), SetLevel2(query, v0_adj, v1_adj, s3, s5, s4, s2), tbb::auto_partitioner());
                continue;
            }
            for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                const IdType v2 = s2[v2_idx];
                VertexSet v2_adj = query.graph->N(v2);
                VertexSet s6 = v2_adj.subtract(v0_adj).subtract(v1_adj);
                if (s6.size() == 0)
                    continue;
                VertexSet s7 = s3.intersect(v2_adj);
                VertexSet s8 = s5.subtract(v2_adj);
                VertexSet s9 = s4.intersect(v2_adj);
                if (s8.size() > query.nested_thresholds[3]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s8.size(), 1), SetLevel3(query, s6, s7, s9, s8), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v3_idx = 0; v3_idx < s8.size(); v3_idx++) { // loop-3 begin
                    const IdType v3 = s8[v3_idx];
                    VertexSet v3_adj = query.graph->N(v3);
                    VertexSet s10 = s6.subtract(v3_adj);
                    VertexSet s11 = s7.intersect(v3_adj);
                    VertexSet s12 = s9.intersect(v3_adj);
                    if (s12.size() > query.nested_thresholds[4]) {
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, s12.size(), 1), SetLevel4(query, s10, s11, s12), tbb::auto_partitioner());
                        continue;
                    }
                    for (size_t v4_idx = 0; v4_idx < s12.size(); v4_idx++) { // loop-4 begin
                        const IdType v4 = s12[v4_idx];
                        VertexSet v4_adj = query.graph->N(v4);
                        VertexSet s13 = s10.subtract(v4_adj);
                        VertexSet s14 = s11.subtract(v4_adj);
                        for (size_t v5_idx = 0; v5_idx < s14.size(); v5_idx++) { // loop-5 begin
                            const IdType v5 = s14[v5_idx];
                            VertexSet v5_adj = query.graph->N(v5);
                            counter += s13.subtract_cnt(v5_adj);
                        }
                    }
                }
            }
        }
    }
};

class SetLevel0 {
  private:
    const QueryContext &query;

  public:
    SetLevel0(const QueryContext &_query) : query{_query} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v0 = r.begin(); v0 < r.end(); v0++) { // loop-0begin
            BenchmarkRootProgress root_progress(query.progress.root(v0));
            VertexSet v0_adj = query.graph->N(v0);
            VertexSet s0 = v0_adj;
            if (s0.size() == 0)
                continue;
            VertexSet s1 = s0.bounded(v0);
            for (size_t v1_idx = 0; v1_idx < s0.size(); v1_idx++) { // loop-1 begin
                const IdType v1 = s0[v1_idx];
                VertexSet v1_adj = query.graph->N(v1);
                VertexSet s2 = v1_adj.subtract(v0_adj);
                if (s2.size() == 0)
                    continue;
                VertexSet s3 = s0.subtract(v1_adj);
                VertexSet s4 = s0.intersect(v1_adj, v1_adj.vid());
                VertexSet s5 = s1.intersect(v1_adj);
                for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                    const IdType v2 = s2[v2_idx];
                    VertexSet v2_adj = query.graph->N(v2);
                    VertexSet s6 = v2_adj.subtract(v0_adj).subtract(v1_adj);
                    if (s6.size() == 0)
                        continue;
                    VertexSet s7 = s3.intersect(v2_adj);
                    VertexSet s8 = s5.subtract(v2_adj);
                    VertexSet s9 = s4.intersect(v2_adj);
                    auto bitmap_rows = BitGraph::build(*query.graph, v2, v2_adj, v2_adj, 5); // bitmap-region build once per anchor
                    if (bitmap_rows) {                                                       // bitmap-enabled continuation
                        const auto &bitgraph = *bitmap_rows;
                        for (size_t v3_idx = 0; v3_idx < s8.size(); v3_idx++) { // loop-3 begin
                            const IdType v3 = s8[v3_idx];
                            VertexSet v3_adj = query.graph->N(v3);
                            VertexSet s10 = s6.subtract(v3_adj);
                            VertexSet s11 = s7.intersect(v3_adj);
                            VertexSet s12 = s9.intersect(v3_adj);
                            Bitmap b12 = Bitmap::from_sorted(bitgraph.universe(), s12.data(), s12.size());
                            Bitmap b10 = Bitmap::from_sorted(bitgraph.universe(), s10.data(), s10.size());
                            Bitmap b11 = Bitmap::from_sorted(bitgraph.universe(), s11.data(), s11.size());
                            { // full bitmap region
                                auto bitmap_execute = [&](auto bitmap_tag) {
                                    constexpr size_t bitmap_words = decltype(bitmap_tag)::value;
                                    return BitLevel4<bitmap_words>(query, bitgraph, bitmap_task_policy, b10, b11, b12)();
                                };
                                counter.add_without_progress(dispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute));
                            } // end bitmap region
                        }
                    } else {                                                    // array-only continuation
                        for (size_t v3_idx = 0; v3_idx < s8.size(); v3_idx++) { // loop-3 begin
                            const IdType v3 = s8[v3_idx];
                            VertexSet v3_adj = query.graph->N(v3);
                            VertexSet s10 = s6.subtract(v3_adj);
                            VertexSet s11 = s7.intersect(v3_adj);
                            VertexSet s12 = s9.intersect(v3_adj);
                            if (s12.size() > query.nested_thresholds[4]) {
                                tbb::parallel_for(tbb::blocked_range<size_t>(0, s12.size(), 1), SetLevel4(query, s10, s11, s12), tbb::auto_partitioner());
                                continue;
                            }
                            for (size_t v4_idx = 0; v4_idx < s12.size(); v4_idx++) { // loop-4 begin
                                const IdType v4 = s12[v4_idx];
                                VertexSet v4_adj = query.graph->N(v4);
                                VertexSet s13 = s10.subtract(v4_adj);
                                VertexSet s14 = s11.subtract(v4_adj);
                                for (size_t v5_idx = 0; v5_idx < s14.size(); v5_idx++) { // loop-5 begin
                                    const IdType v5 = s14[v5_idx];
                                    VertexSet v5_adj = query.graph->N(v5);
                                    counter += s13.subtract_cnt(v5_adj);
                                }
                            }
                        }
                    } // array fallback
                }
            }
        }
    }
};

void plan(const GraphType *graph, Context &ctx) {
    ctx.tick_begin = tbb::tick_count::now();
    ctx.iep_redundency = 0;
    BenchmarkProgress progress(ctx, graph->get_vnum());
    const QueryContext query{graph, ctx, progress, {0, nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), 0}};
    internal::VertexSetPool::configure_for_graph(graph->get_maxdeg());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, graph->get_vnum()), SetLevel0(query), tbb::simple_partitioner());
}
} // namespace minigraph
extern "C" uint64_t graphmini_pattern_size() { return minigraph::pattern_size(); }
extern "C" void graphmini_plan(const minigraph::GraphType *graph, minigraph::Context *ctx) { minigraph::plan(graph, *ctx); }
