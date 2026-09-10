// Naming (D is matching depth; N is an IR set ID):
// sN         : prefix set N (array or bitmap), not matching depth N
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
    const Bitmap &input_s8;
    const Bitmap &input_s9;

  public:
    BitLevel5(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_s8, const Bitmap &input_s9)
        : query(query), bitgraph(bitgraph), policy(policy), input_s8(input_s8), input_s9(input_s9) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_s9, false, *this, policy, 1, 0);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc5, bool parallel_task) const {
        BitmapTaskInput task_s8(input_s8, parallel_task, policy);
        const Bitmap &s8 = task_s8.get();
        BitmapTaskInput task_s9(input_s9, parallel_task, policy);
        const Bitmap &s9 = task_s9.get();
        uint64_t counter = 0;
        for (; bc5.valid(); bc5.advance()) { // bitmap local-index loop
            const auto v5_bit_idx = bc5.position();
            const uint64_t previous_count = counter;
            counter += s8.removed_count<bitmap_words>(v5_bit_idx);
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
    const Bitmap &input_s6;
    const Bitmap &input_s7;

  public:
    BitLevel4(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_s6, const Bitmap &input_s7)
        : query(query), bitgraph(bitgraph), policy(policy), input_s6(input_s6), input_s7(input_s7) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_s7, true, *this, policy, 0, query.nested_thresholds[4]);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc4, bool parallel_task) const {
        BitmapTaskInput task_s6(input_s6, parallel_task, policy);
        const Bitmap &s6 = task_s6.get();
        BitmapTaskInput task_s7(input_s7, parallel_task, policy);
        const Bitmap &s7 = task_s7.get();
        Bitmap s8(bitgraph.universe());
        Bitmap s9(bitgraph.universe());
        uint64_t counter = 0;
        for (; bc4.valid(); bc4.advance()) { // bitmap local-index loop
            const auto v4_bit_idx = bc4.position();
            s8.assign_removed<bitmap_words, true>(s6, v4_bit_idx);
            s9.assign_bounded<bitmap_words, true>(s7, v4_bit_idx);
            counter += BitLevel5<bitmap_words>(query, bitgraph, policy, s8, s9)();
        }
        return counter;
    }
};
// End bitmap level definitions.
class SetLevel4 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s6;
    // Iterate Set
    VertexSet &s7;

  public:
    SetLevel4(const QueryContext &_query, VertexSet &_s6, VertexSet &_s7) : query{_query}, s6{_s6}, s7{_s7} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v4_idx = r.begin(); v4_idx < r.end(); v4_idx++) { // loop-4begin
            const IdType v4 = s7[v4_idx];
            VertexSet v4_adj = query.graph->N(v4);
            VertexSet s8 = s6.remove(v4_adj.vid());
            VertexSet s9 = s7.bounded(v4_adj.vid());
            for (size_t v5_idx = 0; v5_idx < s9.size(); v5_idx++) { // loop-5 begin
                const IdType v5 = s9[v5_idx];
                VertexSet v5_adj = query.graph->N(v5);
                counter += s8.remove_cnt(v5_adj.vid());
            }
        }
    }
};

class SetLevel3 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s4;
    VertexSet &s5;
    // Iterate Set
    VertexSet &s3;

  public:
    SetLevel3(const QueryContext &_query, VertexSet &_s4, VertexSet &_s5, VertexSet &_s3) : query{_query}, s4{_s4}, s5{_s5}, s3{_s3} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v3_idx = r.begin(); v3_idx < r.end(); v3_idx++) { // loop-3begin
            const IdType v3 = s3[v3_idx];
            VertexSet v3_adj = query.graph->N(v3);
            VertexSet s6 = s4.remove(v3_adj.vid());
            VertexSet s7 = s5.intersect(v3_adj);
            if (s7.size() > query.nested_thresholds[4]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s7.size(), 1), SetLevel4(query, s6, s7), tbb::auto_partitioner());
                continue;
            }
            for (size_t v4_idx = 0; v4_idx < s7.size(); v4_idx++) { // loop-4 begin
                const IdType v4 = s7[v4_idx];
                VertexSet v4_adj = query.graph->N(v4);
                VertexSet s8 = s6.remove(v4_adj.vid());
                VertexSet s9 = s7.bounded(v4_adj.vid());
                for (size_t v5_idx = 0; v5_idx < s9.size(); v5_idx++) { // loop-5 begin
                    const IdType v5 = s9[v5_idx];
                    VertexSet v5_adj = query.graph->N(v5);
                    counter += s8.remove_cnt(v5_adj.vid());
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
    VertexSet &s1;
    // Iterate Set
    VertexSet &s2;

  public:
    SetLevel2(const QueryContext &_query, VertexSet &_v0_adj, VertexSet &_v1_adj, VertexSet &_s1, VertexSet &_s2) : query{_query}, v0_adj{_v0_adj}, v1_adj{_v1_adj}, s1{_s1}, s2{_s2} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v2_idx = r.begin(); v2_idx < r.end(); v2_idx++) { // loop-2begin
            const IdType v2 = s2[v2_idx];
            VertexSet v2_adj = query.graph->N(v2);
            VertexSet s3 = v2_adj.remove(v0_adj.vid()).remove(v1_adj.vid());
            if (s3.size() == 0)
                continue;
            VertexSet s4 = s1.remove(v2_adj.vid());
            VertexSet s5 = s2.bounded(v2_adj.vid());
            if (s3.size() > query.nested_thresholds[3]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s3.size(), 1), SetLevel3(query, s4, s5, s3), tbb::auto_partitioner());
                continue;
            }
            for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                const IdType v3 = s3[v3_idx];
                VertexSet v3_adj = query.graph->N(v3);
                VertexSet s6 = s4.remove(v3_adj.vid());
                VertexSet s7 = s5.intersect(v3_adj);
                if (s7.size() > query.nested_thresholds[4]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s7.size(), 1), SetLevel4(query, s6, s7), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v4_idx = 0; v4_idx < s7.size(); v4_idx++) { // loop-4 begin
                    const IdType v4 = s7[v4_idx];
                    VertexSet v4_adj = query.graph->N(v4);
                    VertexSet s8 = s6.remove(v4_adj.vid());
                    VertexSet s9 = s7.bounded(v4_adj.vid());
                    for (size_t v5_idx = 0; v5_idx < s9.size(); v5_idx++) { // loop-5 begin
                        const IdType v5 = s9[v5_idx];
                        VertexSet v5_adj = query.graph->N(v5);
                        counter += s8.remove_cnt(v5_adj.vid());
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
    // Iterate Set
    VertexSet &s0;

  public:
    SetLevel1(const QueryContext &_query, VertexSet &_v0_adj, VertexSet &_s0) : query{_query}, v0_adj{_v0_adj}, s0{_s0} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v1_idx = r.begin(); v1_idx < r.end(); v1_idx++) { // loop-1begin
            const IdType v1 = s0[v1_idx];
            VertexSet v1_adj = query.graph->N(v1);
            VertexSet s1 = s0.remove(v1_adj.vid());
            VertexSet s2 = s0.intersect(v1_adj);
            if (s2.size() > query.nested_thresholds[2]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s2.size(), 1), SetLevel2(query, v0_adj, v1_adj, s1, s2), tbb::auto_partitioner());
                continue;
            }
            for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                const IdType v2 = s2[v2_idx];
                VertexSet v2_adj = query.graph->N(v2);
                VertexSet s3 = v2_adj.remove(v0_adj.vid()).remove(v1_adj.vid());
                if (s3.size() == 0)
                    continue;
                VertexSet s4 = s1.remove(v2_adj.vid());
                VertexSet s5 = s2.bounded(v2_adj.vid());
                if (s3.size() > query.nested_thresholds[3]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s3.size(), 1), SetLevel3(query, s4, s5, s3), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                    const IdType v3 = s3[v3_idx];
                    VertexSet v3_adj = query.graph->N(v3);
                    VertexSet s6 = s4.remove(v3_adj.vid());
                    VertexSet s7 = s5.intersect(v3_adj);
                    if (s7.size() > query.nested_thresholds[4]) {
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, s7.size(), 1), SetLevel4(query, s6, s7), tbb::auto_partitioner());
                        continue;
                    }
                    for (size_t v4_idx = 0; v4_idx < s7.size(); v4_idx++) { // loop-4 begin
                        const IdType v4 = s7[v4_idx];
                        VertexSet v4_adj = query.graph->N(v4);
                        VertexSet s8 = s6.remove(v4_adj.vid());
                        VertexSet s9 = s7.bounded(v4_adj.vid());
                        for (size_t v5_idx = 0; v5_idx < s9.size(); v5_idx++) { // loop-5 begin
                            const IdType v5 = s9[v5_idx];
                            VertexSet v5_adj = query.graph->N(v5);
                            counter += s8.remove_cnt(v5_adj.vid());
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
            auto bitmap_rows = BitGraph::build(*query.graph, v0, v0_adj, v0_adj, 4); // bitmap-region build once per anchor
            for (size_t v1_idx = 0; v1_idx < s0.size(); v1_idx++) {                  // loop-1 begin
                const IdType v1 = s0[v1_idx];
                VertexSet v1_adj = query.graph->N(v1);
                VertexSet s1 = s0.remove(v1_adj.vid());
                VertexSet s2 = s0.intersect(v1_adj);
                for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                    const IdType v2 = s2[v2_idx];
                    VertexSet v2_adj = query.graph->N(v2);
                    VertexSet s3 = v2_adj.remove(v0_adj.vid()).remove(v1_adj.vid());
                    if (s3.size() == 0)
                        continue;
                    VertexSet s4 = s1.remove(v2_adj.vid());
                    VertexSet s5 = s2.bounded(v2_adj.vid());
                    for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                        const IdType v3 = s3[v3_idx];
                        VertexSet v3_adj = query.graph->N(v3);
                        VertexSet s6 = s4.remove(v3_adj.vid());
                        VertexSet s7 = s5.intersect(v3_adj);
                        std::optional<Bitmap> bitmap_s7;
                        std::optional<Bitmap> bitmap_s6;
                        if (bitmap_rows) {
                            bitmap_s7.emplace(Bitmap::from_sorted(bitmap_rows->universe(), s7.data(), s7.size()));
                        }
                        if (bitmap_rows) {
                            bitmap_s6.emplace(Bitmap::from_sorted(bitmap_rows->universe(), s6.data(), s6.size()));
                        }
                        if (bitmap_rows) { // full bitmap region
                            const auto &bitgraph = *bitmap_rows;
                            auto bitmap_execute = [&](auto bitmap_tag) {
                                constexpr size_t bitmap_words = decltype(bitmap_tag)::value;
                                return BitLevel4<bitmap_words>(query, bitgraph, bitmap_task_policy, *bitmap_s6, *bitmap_s7)();
                            };
                            counter.add_without_progress(dispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute));
                        } else {
                            if (s7.size() > query.nested_thresholds[4]) {
                                tbb::parallel_for(tbb::blocked_range<size_t>(0, s7.size(), 1), SetLevel4(query, s6, s7), tbb::auto_partitioner());
                                continue;
                            }
                            for (size_t v4_idx = 0; v4_idx < s7.size(); ++v4_idx) {
                                const IdType v4 = s7[v4_idx];
                                VertexSet v4_adj = query.graph->N(v4);
                                VertexSet s8 = s6.remove(v4_adj.vid());
                                VertexSet s9 = s7.bounded(v4_adj.vid());
                                for (size_t v5_idx = 0; v5_idx < s9.size(); v5_idx++) { // loop-5 begin
                                    const IdType v5 = s9[v5_idx];
                                    VertexSet v5_adj = query.graph->N(v5);
                                    counter += s8.remove_cnt(v5_adj.vid());
                                }
                            }
                        } // array fallback
                    }
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
