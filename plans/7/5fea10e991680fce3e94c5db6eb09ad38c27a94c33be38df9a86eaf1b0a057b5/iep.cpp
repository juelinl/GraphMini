// Naming (D is matching depth; N is an IR set ID):
// sN         : array-backed vertex set with IR set ID N
// bN         : bitmap with IR set ID N (same ID as its array representation)
// vD         : global vertex ID matched at depth D
// vD_idx     : position in the prefix set iterated at depth D
// vD_adj     : adjacency list or bitmap row of vD
// vD_bit_idx : position of vD in the current bitmap universe
// SetLevelD / BitLevelD : array / bitmap task at matching depth D

#include "plan.h"
#include "backend/benchmark_progress.h"
#include <array>
#include "runtime/nested_policy.h"
namespace minigraph {
// Borrowed per-query state; all task joins complete before plan returns.
struct QueryContext {
    const Graph *const graph;
    Context &ctx;
    BenchmarkProgress &progress;
    const std::array<size_t, 6> nested_thresholds;
};
uint64_t pattern_size() { return 7; }
class SetLevel3 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s4;
    // Iterate Set
    VertexSet &s3;

  public:
    SetLevel3(const QueryContext &_query, VertexSet &_s4, VertexSet &_s3) : query{_query}, s4{_s4}, s3{_s3} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v3_idx = r.begin(); v3_idx < r.end(); v3_idx++) { // loop-3begin
            const IdType v3 = s3[v3_idx];
            VertexSet v3_adj = query.graph->N(v3);
            VertexSet s5 = s4.intersect(v3_adj);
            for (size_t v4_idx = 0; v4_idx < s5.size(); v4_idx++) { // loop-4 begin
                const IdType v4 = s5[v4_idx];
                VertexSet v4_adj = query.graph->N(v4);
                VertexSet s6 = s5.bounded(v4_adj.vid());
                counter += 1ll * s6.size() * s6.size();
                /* Val: 1 | Group: (0),(1) | Comp: |VSet(6)|*|VSet(6)| */
                counter += -1ll * s6.size();
                /* Val: -1 | Group: (0 1) | Comp: |VSet(6) & VSet(6)| */
            }
        }
    }
};

class SetLevel2 {
  private:
    const QueryContext &query;
    // Parent Intermediates
    VertexSet &s2;
    // Iterate Set
    VertexSet &s1;

  public:
    SetLevel2(const QueryContext &_query, VertexSet &_s2, VertexSet &_s1) : query{_query}, s2{_s2}, s1{_s1} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v2_idx = r.begin(); v2_idx < r.end(); v2_idx++) { // loop-2begin
            const IdType v2 = s1[v2_idx];
            VertexSet v2_adj = query.graph->N(v2);
            VertexSet s3 = s1.bounded(v2_adj.vid());
            VertexSet s4 = s2.intersect(v2_adj);
            if (s3.size() > query.nested_thresholds[3]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s3.size(), 1), SetLevel3(query, s4, s3), tbb::auto_partitioner());
                continue;
            }
            for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                const IdType v3 = s3[v3_idx];
                VertexSet v3_adj = query.graph->N(v3);
                VertexSet s5 = s4.intersect(v3_adj);
                for (size_t v4_idx = 0; v4_idx < s5.size(); v4_idx++) { // loop-4 begin
                    const IdType v4 = s5[v4_idx];
                    VertexSet v4_adj = query.graph->N(v4);
                    VertexSet s6 = s5.bounded(v4_adj.vid());
                    counter += 1ll * s6.size() * s6.size();
                    /* Val: 1 | Group: (0),(1) | Comp: |VSet(6)|*|VSet(6)| */
                    counter += -1ll * s6.size();
                    /* Val: -1 | Group: (0 1) | Comp: |VSet(6) & VSet(6)| */
                }
            }
        }
    }
};

class SetLevel1 {
  private:
    const QueryContext &query;
    // Iterate Set
    VertexSet &s0;

  public:
    SetLevel1(const QueryContext &_query, VertexSet &_s0) : query{_query}, s0{_s0} {};
    void operator()(const tbb::blocked_range<size_t> &r) const {
        auto &ctx = query.ctx;
        const int worker_id = tbb::this_task_arena::current_thread_index();
        cc &counter = ctx.per_thread_result.at(worker_id);
        for (size_t v1_idx = r.begin(); v1_idx < r.end(); v1_idx++) { // loop-1begin
            const IdType v1 = s0[v1_idx];
            VertexSet v1_adj = query.graph->N(v1);
            VertexSet s1 = s0.bounded(v1_adj.vid());
            VertexSet s2 = s0.intersect(v1_adj, v1_adj.vid());
            if (s1.size() > query.nested_thresholds[2]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s1.size(), 1), SetLevel2(query, s2, s1), tbb::auto_partitioner());
                continue;
            }
            for (size_t v2_idx = 0; v2_idx < s1.size(); v2_idx++) { // loop-2 begin
                const IdType v2 = s1[v2_idx];
                VertexSet v2_adj = query.graph->N(v2);
                VertexSet s3 = s1.bounded(v2_adj.vid());
                VertexSet s4 = s2.intersect(v2_adj);
                if (s3.size() > query.nested_thresholds[3]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s3.size(), 1), SetLevel3(query, s4, s3), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                    const IdType v3 = s3[v3_idx];
                    VertexSet v3_adj = query.graph->N(v3);
                    VertexSet s5 = s4.intersect(v3_adj);
                    for (size_t v4_idx = 0; v4_idx < s5.size(); v4_idx++) { // loop-4 begin
                        const IdType v4 = s5[v4_idx];
                        VertexSet v4_adj = query.graph->N(v4);
                        VertexSet s6 = s5.bounded(v4_adj.vid());
                        counter += 1ll * s6.size() * s6.size();
                        /* Val: 1 | Group: (0),(1) | Comp: |VSet(6)|*|VSet(6)| */
                        counter += -1ll * s6.size();
                        /* Val: -1 | Group: (0 1) | Comp: |VSet(6) & VSet(6)| */
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
            if (s0.size() > query.nested_thresholds[1]) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, s0.size(), 1), SetLevel1(query, s0), tbb::auto_partitioner());
                continue;
            }
            for (size_t v1_idx = 0; v1_idx < s0.size(); v1_idx++) { // loop-1 begin
                const IdType v1 = s0[v1_idx];
                VertexSet v1_adj = query.graph->N(v1);
                VertexSet s1 = s0.bounded(v1_adj.vid());
                VertexSet s2 = s0.intersect(v1_adj, v1_adj.vid());
                if (s1.size() > query.nested_thresholds[2]) {
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, s1.size(), 1), SetLevel2(query, s2, s1), tbb::auto_partitioner());
                    continue;
                }
                for (size_t v2_idx = 0; v2_idx < s1.size(); v2_idx++) { // loop-2 begin
                    const IdType v2 = s1[v2_idx];
                    VertexSet v2_adj = query.graph->N(v2);
                    VertexSet s3 = s1.bounded(v2_adj.vid());
                    VertexSet s4 = s2.intersect(v2_adj);
                    if (s3.size() > query.nested_thresholds[3]) {
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, s3.size(), 1), SetLevel3(query, s4, s3), tbb::auto_partitioner());
                        continue;
                    }
                    for (size_t v3_idx = 0; v3_idx < s3.size(); v3_idx++) { // loop-3 begin
                        const IdType v3 = s3[v3_idx];
                        VertexSet v3_adj = query.graph->N(v3);
                        VertexSet s5 = s4.intersect(v3_adj);
                        for (size_t v4_idx = 0; v4_idx < s5.size(); v4_idx++) { // loop-4 begin
                            const IdType v4 = s5[v4_idx];
                            VertexSet v4_adj = query.graph->N(v4);
                            VertexSet s6 = s5.bounded(v4_adj.vid());
                            counter += 1ll * s6.size() * s6.size();
                            /* Val: 1 | Group: (0),(1) | Comp: |VSet(6)|*|VSet(6)| */
                            counter += -1ll * s6.size();
                            /* Val: -1 | Group: (0 1) | Comp: |VSet(6) & VSet(6)| */
                        }
                    }
                }
            }
        }
    }
};

void plan(const GraphType *graph, Context &ctx) {
    ctx.tick_begin = tbb::tick_count::now();
    ctx.iep_redundency = 2;
    BenchmarkProgress progress(ctx, graph->get_vnum());
    const QueryContext query{graph, ctx, progress, {0, nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), nested_threshold(graph->num_vertex, graph->num_edge, graph->max_degree, 4), 0, 0}};
    internal::VertexSetPool::configure_for_graph(graph->get_maxdeg());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, graph->get_vnum()), SetLevel0(query), tbb::simple_partitioner());
}
} // namespace minigraph
extern "C" uint64_t graphmini_pattern_size() { return minigraph::pattern_size(); }
extern "C" void graphmini_plan(const minigraph::GraphType *graph, minigraph::Context *ctx) { minigraph::plan(graph, *ctx); }
