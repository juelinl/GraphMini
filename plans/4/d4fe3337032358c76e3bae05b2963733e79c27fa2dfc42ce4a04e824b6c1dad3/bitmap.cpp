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
    const std::array<size_t, 3> nested_thresholds;
};
static const auto bitmap_task_policy = BitmapTaskPolicy::from_environment();
uint64_t pattern_size() { return 4; }
// Bitmap level definitions: borrowed inputs; invocation-local scratch.
template <size_t bitmap_words>
class BitLevel2 {
    const QueryContext &query;
    const BitGraph &bitgraph;
    const BitmapTaskPolicy &policy;
    const Bitmap &input_b2;

  public:
    BitLevel2(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_b2)
        : query(query), bitgraph(bitgraph), policy(policy), input_b2(input_b2) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_b2, false, *this, policy, 1, 0);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc2, bool parallel_task) const {
        BitmapTaskInput task_b2(input_b2, parallel_task, policy);
        const Bitmap &b2 = task_b2.get();
        uint64_t counter = 0;
        for (; bc2.valid(); bc2.advance()) { // bitmap local-index loop
            const auto v2_bit_idx = bc2.position();
            const uint64_t previous_count = counter;
            counter += b2.bounded_count<bitmap_words>(v2_bit_idx);
            query.progress.add_bitmap_matches(counter - previous_count);
        }
        return counter;
    }
};
template <size_t bitmap_words>
class BitLevel1 {
    const QueryContext &query;
    const BitGraph &bitgraph;
    const BitmapTaskPolicy &policy;
    const Bitmap &input_b0;
    const Bitmap &input_b1;

  public:
    BitLevel1(const QueryContext &query, const BitGraph &bitgraph, const BitmapTaskPolicy &policy, const Bitmap &input_b0, const Bitmap &input_b1)
        : query(query), bitgraph(bitgraph), policy(policy), input_b0(input_b0), input_b1(input_b1) {}
    uint64_t operator()() const {
        return bitmap_for_each(input_b1, true, *this, policy, 0, query.nested_thresholds[1]);
    }
    template <class Cursor>
    uint64_t operator()(Cursor bc1, bool parallel_task) const {
        BitmapTaskInput task_b0(input_b0, parallel_task, policy);
        const Bitmap &b0 = task_b0.get();
        BitmapTaskInput task_b1(input_b1, parallel_task, policy);
        const Bitmap &b1 = task_b1.get();
        Bitmap b2(bitgraph.universe());
        uint64_t counter = 0;
        for (; bc1.valid(); bc1.advance()) { // bitmap local-index loop
            const auto v1_bit_idx = bc1.position();
            const auto v1_adj = bitgraph.local_row(v1_bit_idx);
            b2.assign_intersection<bitmap_words, true>(b0, v1_adj);
            counter += BitLevel2<bitmap_words>(query, bitgraph, policy, b2)();
        }
        return counter;
    }
};
// End bitmap level definitions.
class SetLevel1 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s0;
    // Iterate Set
    VertexSet &s1;

  public:
    SetLevel1(const QueryContext &_query, VertexSet &_s0, VertexSet &_s1) : query{_query}, s0{_s0}, s1{_s1} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v1_idx = r.begin(); v1_idx < r.end(); v1_idx++) { // loop-1begin
            const IdType v1 = s1[v1_idx];
            VertexSet v1_adj = query.graph->N(v1);
            VertexSet s2 = s0.intersect(v1_adj);
            for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                const IdType v2 = s2[v2_idx];
                VertexSet v2_adj = query.graph->N(v2);
                counter += s2.bounded_cnt(v2_adj.vid());
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
            auto bitmap_rows = BitGraph::build(*query.graph, v0, v0_adj, v0_adj, 3); // bitmap-region build once per anchor
            if (bitmap_rows) {                                                       // bitmap-enabled continuation
                const auto &bitgraph = *bitmap_rows;
                Bitmap b1 = Bitmap::from_sorted(bitgraph.universe(), s1.data(), s1.size());
                Bitmap b0 = Bitmap::from_sorted(bitgraph.universe(), s0.data(), s0.size());
                { // full bitmap region
                    auto bitmap_execute = [&](auto bitmap_tag) {
                        constexpr size_t bitmap_words = decltype(bitmap_tag)::value;
                        return BitLevel1<bitmap_words>(query, bitgraph, bitmap_task_policy, b0, b1)();
                    };
                    counter.add_without_progress(dispatch_bitmap_words(bitgraph.universe().size(), bitmap_execute));
                } // end bitmap region
            } else { // array-only continuation
                if (s1.size() > query.nested_thresholds[1]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s1.size(), 1), SetLevel1(query, s0, s1), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v1_idx = 0; v1_idx < s1.size(); v1_idx++) { // loop-1 begin
                    const IdType v1 = s1[v1_idx];
                    VertexSet v1_adj = query.graph->N(v1);
                    VertexSet s2 = s0.intersect(v1_adj);
                    for (size_t v2_idx = 0; v2_idx < s2.size(); v2_idx++) { // loop-2 begin
                        const IdType v2 = s2[v2_idx];
                        VertexSet v2_adj = query.graph->N(v2);
                        counter += s2.bounded_cnt(v2_adj.vid());
                    }
                }
            } // array fallback
        }
    }
};

void plan(const GraphType *graph, Context &ctx) {
    ctx.tick_begin = tbb::tick_count::now();
    ctx.iep_redundency = 0;
    BenchmarkProgress progress(ctx, graph->get_vnum());
    const QueryContext query{graph, ctx, progress, {0, nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), 0}};
    internal::VertexSetPool::configure_for_graph(graph->get_maxdeg());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, graph->get_vnum()), SetLevel0(query), tbb::simple_partitioner());
}
} // namespace minigraph
extern "C" uint64_t graphmini_pattern_size() { return minigraph::pattern_size(); }
extern "C" void graphmini_plan(const minigraph::GraphType *graph, minigraph::Context *ctx) { minigraph::plan(graph, *ctx); }
