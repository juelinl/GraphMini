# Compiler organization

The public `gen_code` API in `include/codegen.h` remains the entry point for the
CLI, Python API, and profiler. Query semantics and configuration remain local
to each plan; there is no global query-mode switch.

The compiler separates logical planning, execution decisions, and C++ writing:

1. `vertex_induced.cpp`, `edge_induced.cpp`, and `edge_induced_iep.cpp` select query
   semantics. `planning.cpp` adapts the scheduler into the logical `PlanIR`.
2. `passes/auxiliary_graphs.cpp` selects auxiliary graphs in the logical plan.
3. `passes/lower_execution.cpp` resolves set inputs, constraints, MiniGraph reads,
   terminal cardinalities, and IEP factors into `ExecutionIR`.
4. `passes/lower_minigraphs.cpp` resolves eager construction, parent builds,
   direct versus explicit index mapping, and structured reuse estimates.
5. `passes/lower_loops.cpp` resolves adjacency reads, task captures, and the
   existing nested-task threshold policy.
6. `verify_execution` checks the lowered plan before `codegen/` renders C++.

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
Runtime tests compare generated kernels with a symmetry-normalized exhaustive
oracle; see `tests/runtime_smoke.py` and `tests/runtime_large.py`.

This checkpoint preserves the existing execution heuristics and generated C++.
The nested-task degree calculation additionally handles zero average degree
without integer division by zero. The innermost-two-loop policy is unchanged.

The verifier checks set definition ordering and basic operand, build-input,
capture, and IEP validity. It is not a full ownership, alias, or control-flow
verifier. OpenMP/TBB loop scaffolding and logical comments still use `PlanIR`;
the writer's capture adapters also resolve physical IDs back to logical objects.
The next structural step is an explicit loop/block IR and shared body traversal,
followed by liveness-based materialization and ownership checks. Do not introduce
new performance heuristics as part of that mechanical conversion.

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
