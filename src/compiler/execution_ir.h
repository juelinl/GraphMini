#pragma once
#include "ir.h"
#include "compiler/representation.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace minigraph {
// Physical set expressions contain no C++ source and no query-mode decisions.
enum class SetSource { Prefix, GraphAdjacency, MiniGraphAdjacency };
struct SetReference {
    SetSource source;
    int id;
};
struct VertexReference {
    // A selected loop vertex, or the owner of an adjacency set.
    int depth;
    std::optional<SetReference> adjacency;
};
enum class SetOpcode { Intersect, DifferenceExcludingOwner, Bound, Remove };
struct SetStep {
    SetOpcode opcode;
    std::optional<SetReference> rhs;
    std::optional<VertexReference> vertex;
    std::optional<VertexReference> upper_bound;
};
enum class SetResult { Materialize, Count, MaterializeThenCount };
struct SetExecution {
    int id;
    int depth;
    SetReference input;
    std::vector<SetStep> steps;
    SetResult result{SetResult::Materialize};
    bool guard_empty{false};
    std::vector<std::string> rules;
};
struct IEPTerm {
    int coefficient;
    // Each factor is the cardinality of the intersection of these prefix sets.
    std::vector<std::vector<int>> factors;
};
struct VisitFactor {
    std::optional<int> set_id;
    double scale;
};
struct ReuseEstimate {
    int initial_set;
    std::vector<VisitFactor> factors;
    int uses;
};
struct MiniGraphExecution {
    int id, depth, vertices, intersect, iter;
    std::optional<int> parent;
    bool eager, bounded, parallel, estimate_reuse;
    std::vector<ReuseEstimate> reuse;
    // A restriction-only prefix preserves positions; other iterators need
    // indices().
    std::map<int, bool> direct_indices;
};
struct LoopExecution {
    bool read_adjacency{false};
    bool spawn_nested{false};
    bool runtime_threshold{false};
    int threshold_factor{4};
    int average_degree{0};
    bool cap_threshold{false};
    int grain_size{1};
    std::vector<int> captured_sets, captured_minigraphs;
    std::set<int> captured_adjacencies;
};
// Array live-ins remain available for memory-budget fallback. Bitmap copies and
// restricted rows are owned by this region, outside the terminal matching loop.
struct BitmapRegionExecution {
    int entry_depth, conversion_depth, anchor_depth;
    // Rows cover the full universe, so every later selected vertex has a row.
    std::vector<int> live_ins, count_ops;
    int iterator_set{-1}; // Bitmap live-in traversed by the last explicit matching loop.
};
struct ExecutionIR {
    int serial_loop_boundary{1};
    DomainAnalysis domains;
    RepresentationPlan representations;
    std::map<int, SetExecution> sets;
    std::vector<IEPTerm> iep;
    std::map<int, MiniGraphExecution> minigraphs;
    std::vector<LoopExecution> loops;
    std::optional<BitmapRegionExecution> bitmap_region;
    std::string bitmap_reason;
};

ExecutionIR lower_execution(const PlanIR &plan);
void lower_minigraphs(const PlanIR &plan, ExecutionIR &execution);
void lower_loops(const PlanIR &plan, ExecutionIR &execution);
void lower_bitmap_region(const PlanIR &plan, ExecutionIR &execution);
void verify_bitmap_region(const PlanIR &plan, const ExecutionIR &execution);
void verify_execution(const ExecutionIR &execution, const PlanIR &plan);
std::string dump_execution(const ExecutionIR &execution);
} // namespace minigraph
