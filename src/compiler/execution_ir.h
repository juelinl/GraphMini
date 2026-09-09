#pragma once
#include "compiler/ir.h"
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
// Convert only IEP factor inputs, not a full graph. Each factor shares one
// proven neighborhood universe; cardinalities are reused across IEP terms.
struct IEPBitmapExecution {
    int anchor_depth;
    std::vector<int> inputs;
    std::vector<std::vector<int>> factors;
    std::optional<int> universe_set; // Common materialized ancestor, otherwise N(anchor).
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
// Array live-ins remain available for memory-budget fallback. A full region
// owns fixed slots for all materialized prefixes; otherwise only terminal-loop
// inputs are converted. Both use one immutable, full-universe row store.
struct BitmapProjectionPair {
    int positive, negative, local_depth, external_depth;
    std::vector<int> fallback_sets;
    bool operator==(const BitmapProjectionPair &other) const {
        return positive == other.positive && negative == other.negative &&
               local_depth == other.local_depth && external_depth == other.external_depth &&
               fallback_sets == other.fallback_sets;
    }
};
struct BitmapBinding {
    int set_id;
    int slot;
    // Initialize after this depth's set definitions/guards, on every visit.
    // Older live-ins bind at region entry; newly defined inputs bind in-scope.
    int depth;
};
struct BitmapRegionExecution {
    int entry_depth, conversion_depth, anchor_depth;
    // Immutable rows depend only on the anchor's full graph neighborhood.
    // Their lifetime is independent of the legal bitmap execution boundary.
    int build_depth{-1};
    // Rows cover the full universe, so every vertex selected inside the bitmap
    // suffix has a row. Vertices between build and entry may lie outside it.
    std::vector<int> live_ins, count_ops;
    int iterator_set{-1}; // Bitmap live-in traversed by the last explicit matching loop.
    bool full_region{false};
    std::vector<int> full_sets, full_live_ins; // fixed slots and boundary conversions
    std::optional<BitmapProjectionPair> projection_pair;
    // Resolved physical layout, preserving full_sets/live_ins ordering.
    std::map<int, int> slots; // set ID -> candidate slot, including private outputs
    // Live-in destinations initialized from arrays, or jointly by projection_pair.
    std::vector<BitmapBinding> bindings;
};
struct ExecutionIR {
    int serial_loop_boundary{1};
    DomainAnalysis domains;
    RepresentationPlan representations;
    std::map<int, SetExecution> sets;
    std::vector<IEPTerm> iep;
    std::optional<IEPBitmapExecution> iep_bitmap;
    std::map<int, MiniGraphExecution> minigraphs;
    std::vector<LoopExecution> loops;
    std::optional<BitmapRegionExecution> bitmap_region;
    std::string bitmap_reason;
};

ExecutionIR lower_execution(const PlanIR &plan);
void lower_minigraphs(const PlanIR &plan, ExecutionIR &execution);
void lower_loops(const PlanIR &plan, ExecutionIR &execution);
void lower_bitmap_region(const PlanIR &plan, ExecutionIR &execution);
// Resolve physical slots and initialization scopes after selecting a region.
void lower_bitmap_bindings(const ExecutionIR &execution, BitmapRegionExecution &region);
void verify_bitmap_bindings(const ExecutionIR &execution, const BitmapRegionExecution &region);
std::optional<IEPBitmapExecution> plan_iep_bitmap(const PlanIR &plan, const ExecutionIR &execution);
void verify_bitmap_region(const PlanIR &plan, const ExecutionIR &execution);
void verify_execution(const ExecutionIR &execution, const PlanIR &plan);
std::string dump_execution(const ExecutionIR &execution);
} // namespace minigraph
