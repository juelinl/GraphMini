# Compiler organization

The public `gen_code` API in `include/codegen.h` remains the entry point for the
CLI, Python API, and profiler. Query semantics and configuration remain local
to each plan; there is no global query-mode switch.

The compiler separates logical planning, execution decisions, and C++ writing:

1. `vertex_induced.cpp`, `edge_induced.cpp`, and `edge_induced_iep.cpp` select query
   semantics. `planning.cpp` adapts the scheduler into `ScheduledConstraints`.
2. `passes/auxiliary_graphs.cpp` selects auxiliary graphs in the logical plan.
3. `passes/lower_execution.cpp` resolves set inputs, constraints, MiniGraph reads,
   terminal cardinalities, and IEP factors into `ExecutionIR`.
4. `passes/lower_minigraphs.cpp` resolves eager construction, parent builds,
   direct versus explicit index mapping, and structured reuse estimates.
5. `passes/lower_loops.cpp` resolves adjacency reads, task captures, and the
   existing nested-task threshold policy.
6. `verify_execution` checks the lowered plan before `codegen/` renders C++.

## Separated responsibilities

`PlanIR` is now a pipeline aggregate, with explicit components rather than a
single bag of logical and physical fields:

| Component | Responsibility |
|---|---|
| `query: QueryIR` | Original adjacency matrix and vertex-/edge-induced semantics. IEP is not a query semantic. |
| `logical: ScheduledConstraints` | Scheduled adjacency, matching order, prefix constraints, and iteration sets. No storage format or graph statistics. |
| `context: PlanningContext` | Input-graph statistics and compiler options. |
| `counting: InclusionExclusionPlan` | IEP factors, suffix strategy, and symmetry divisor. |
| `auxiliary: AuxiliaryGraphPlan` | Selected MiniGraphs, uses, and bounds. |
| `ExecutionIR` | Concrete operations, MiniGraph construction, parallel policy, and representation choices. |

Parent-selection helpers remain on the pipeline aggregate because they inspect
multiple planning components. Serial-loop boundaries and terminal count policy
now live in lowering, not in the logical IR.

`passes/representation.cpp` analyzes only `ScheduledConstraints`. `DomainAnalysis`
records proven containment independently of `RepresentationPlan`, which selects
storage. The execution plan retains both for verification and inspection.

## Next implementation target: local-universe bitmaps

For each set, domain analysis records every matched anchor `a` for which the
set is a subset of `N(a)`. A nonedge does not establish containment; bounds and
exclusions do not invalidate containment already established by an edge.

It also identifies scheduled suffixes with at least two remaining vertices all
adjacent to the same matched anchor. All legal anchors are retained. These are
**opportunities**, not instructions to allocate bitmaps: IEP may eliminate some
suffix loops, and profitability still depends on universe size and reuse.

A universe ID names an anchor's scheduled loop depth. Its runtime identity is
scoped to that anchor's current binding; it must not survive rebinding the anchor.
Positions follow the sorted global IDs in `N(a)`, enabling prefix masks for
symmetry bounds. Different universe IDs cannot be combined directly.

The current selector always chooses `SortedArray` with no universe conversion.
The verifier rejects `UniverseBitmap` even with a valid containment proof because
bitmap kernels and conversions are not implemented yet. Generated C++ is unchanged.

The next implementation should:

1. Select an active innermost count-only region and one common anchor.
2. Construct/reuse `N(v) intersect N(a)` masks, with explicit array-to-bitmap
   conversion and a reverse position-to-global-ID map.
3. Lower intersection/difference/counting to word operations and popcount,
   preserving owner exclusion, distinctness, strict bounds, and unused-tail masking.
4. Verify universe compatibility and lifetime across loops/tasks; keep arrays
   when conversion or sparse scanning is cheaper.
5. Compare against both array execution and the symmetry-normalized oracle,
   including word-boundary sizes, empty masks, multiple legal anchors, and IEP.

Do not infer bitmap legality from MiniGraph index reuse: the two optimizations
have separate proofs and lifetime requirements.

## Where optimizations belong

Execution IR contains typed operations and references, not C++ expressions.
Change a lowering rule when changing execution strategy; change codegen only
when changing how a decided operation is expressed in C++.

For example, `DifferenceExcludingOwner` explicitly means
`A \\ B \\ {owner(B)}`. Its optional upper bound is a separate operand.
`SetResult::Count` requests the count kernel for the final operation in a chain;
`MaterializeThenCount` retains an intermediate set and its empty guard.
These distinctions must not be inferred again by the writer.

MiniGraph index reuse is recorded per iterator. Equal edge prefixes at the same
depth permit direct indexing through restriction-only prefix views; otherwise
the plan requests an explicit `indices()` mapping. Cost estimates store set-size
factors and numeric selectivities rather than source-code fragments.

`SetExecution::rules` explains set-lowering choices. `dump_execution()` prints
sets, bounds, owner exclusions, result modes, MiniGraph policy, reuse estimates,
captures, and IEP factors. Generate example dumps with:

```sh
build-inline-pch/bin/execution_ir_regression /tmp/graphmini-ir
```

## Validation and current boundaries

`compiler_regression` covers 1,800 code-generation configurations.
`execution_ir_regression` checks 75 lowered plans, including six- and seven-vertex
patterns, deterministic dumps, and rejection of malformed operands/references.
`representation_regression` checks common-universe stars, multiple clique anchors,
negative path/nonedge cases, restriction invariance, and rejection of unproved
annotations or unsupported bitmap storage. Execution tests additionally check
that auxiliary planning leaves domain facts unchanged.
Runtime tests compare generated kernels with a symmetry-normalized exhaustive
oracle; see `tests/runtime_smoke.py` and `tests/runtime_large.py`.

This checkpoint preserves the existing execution heuristics and generated C++.
The nested-task degree calculation additionally handles zero average degree
without integer division by zero. The innermost-two-loop policy is unchanged.

The verifier checks set definition ordering and basic operand, build-input,
capture, and IEP validity. It is not a full ownership, alias, or control-flow
verifier. OpenMP/TBB loop scaffolding and logical comments still use `PlanIR`;
the writer's capture adapters also resolve physical IDs back to logical objects.
An explicit loop/block IR and shared body traversal remain future structural work,
along with liveness-based materialization and ownership checks. Bitmap region
selection must respect the current IEP and loop structure until that migration.

Runtime builds and Python plan caching are unchanged. Runtime tests require
NumPy and the Python interpreter used for the build, with CMake, the build tool,
and clang-format on PATH. Run them serially per checkout because compilation
updates `src/codegen_output/plan.cpp`; concurrent compilation is not supported.

### IEP symmetry correction regression

The original `0c19179` build overcounts a four-vertex star in `edge_iep` mode:
22 instead of 11 on the six-vertex test graph. Both standalone schedulers used a
hard-coded IEP divisor of one. `include/iep_redundancy.hpp` now computes the exact
ratio of rank assignments with and without suffix ordering constraints, retaining
transitive prefix bounds. This supplies the existing runtime division with the
correct factor without requiring a synthetic graph or the GraphPi matcher.

The exhaustive test covers four patterns (including stars requiring factors two
and six, and a complete bipartite pattern), all three schedulers, three query
modes, three pruning modes, OpenMP and nested TBB, and one/two threads: 432
executions checked against independent exhaustive counts.

Use `--results counts.json` to record every case even when oracle checks fail.
Use `--baseline counts.json` with a second build to check behavior preservation
separately from mathematical correctness; baseline files must use the same test
matrix. Before this bug fix, all 72 executions in the original smoke test and
all 1,800 generated-source snapshots matched the pre-refactor build. The IEP
divisor and affected counts intentionally differ now.
