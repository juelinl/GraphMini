# Compiler organization

The public `gen_code` API in `include/codegen.h` remains the entry point for
the CLI, Python API, and profiler. `compiler.cpp` dispatches to a query-mode
compiler, applies auxiliary-graph planning, and selects a C++ writer.

- `vertex_induced.cpp`: vertex-induced planning, including nonedge constraints.
- `edge_induced.cpp`: edge-induced planning.
- `edge_induced_iep.cpp`: edge-induced planning with inclusion-exclusion setup.
- `planning.cpp`: shared scheduling adapter and prefix-set IR construction.
- `ir.cpp`: IR operations. Each vertex set carries its query semantics; each
  plan carries its configuration. No global query-mode switch is used.
- `passes/auxiliary_graphs.cpp`: constructs and prunes auxiliary-graph IR.
- `codegen/cpp.cpp`: shared C++ operations and inclusion-exclusion expressions.
- `codegen/openmp.cpp`: OpenMP execution structure.
- `codegen/tbb.cpp`: TBB top-level and nested execution structure.

`CppCodegen` owns the configuration for one source-generation operation.
Query-mode compilers share the planner and writers rather than duplicating
complete compiler implementations. The current set-operation IR still contains
mode-dependent lowering; this refactor preserves those algorithms.

Generated plans continue to use the existing C++17/PCH runtime build and loader.
Runtime build directories and the Python plan cache are unchanged; concurrent
runtime compilation is not yet supported by this refactor.

## Verification

Configure with `-DGRAPHMINI_BUILD_TESTS=ON`, build `compiler_regression`, then run
`ctest --test-dir build --output-on-failure`. The regression executable covers
all query modes, schedulers, pruning modes, parallel modes, and profiling modes.
An optional output-directory argument writes generated sources for comparison
with a baseline build.

For end-to-end counting checks, build `pygraphmini` and run:

```sh
PYTHONPATH=build/lib python tests/runtime_smoke.py
```

The active Python must have NumPy and match the interpreter used for the build;
`cmake`, the build tool, and `clang-format` must be on PATH. Runtime compilation
updates the existing generated `src/codegen_output/plan.cpp` file.

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
