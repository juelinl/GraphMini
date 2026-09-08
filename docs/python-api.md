# Python API reference

[Back to GraphMini](../README.md)

Run examples from the repository root. Examples using `build` assume that build
directory; substitute `build-conda` if you followed the recommended installation.

## Python API Overview

The extension module is named `pygraphmini`.

### Core objects

- `pygraphmini.Graph`
  - `Graph.from_preprocessed(graph_dir, reorder_by_degree=False)`
  - `Graph.from_csr(indptr, indices, offsets=None, triangles=None, reorder_by_degree=False)`
- `pygraphmini.CompiledPlan`
  - returned by `pygraphmini.compile_plan(...)`
  - executes a compiled shared module in-process
- `pygraphmini.RunResult`
  - structured result object returned by `CompiledPlan.run(...)`

### Python API Input Reference

#### `Graph.from_preprocessed(graph_dir, reorder_by_degree=False)`

- `graph_dir`
  - type: `str`
  - path to a preprocessed GraphMini graph directory
  - the directory is expected to contain GraphMini's binary graph files such as `meta.txt`, `indptr_u64.bin`, `offset_u64.bin`, triangle counts, and the compiled `indices` file matching the current vertex-id width
- `reorder_by_degree`
  - type: `bool`
  - default: `False`
  - when `True`, GraphMini reloads the graph and remaps vertex ids so higher-degree data-graph vertices receive smaller ids

#### `Graph.from_csr(indptr, indices, offsets=None, triangles=None, reorder_by_degree=False)`

- `indptr`
  - type: 1-D NumPy array of `np.uint64`
  - shape: `num_vertices + 1`
  - CSR row-pointer array
  - `indptr[0]` must be `0`
  - `indptr[-1]` must equal `len(indices)`
  - must be non-decreasing
- `indices`
  - type: 1-D NumPy integer array compatible with GraphMini vertex ids
  - current build expects 32-bit vertex ids in practice
  - stores all adjacency lists concatenated together in CSR order
  - every value must be in `[0, num_vertices)`
  - adjacency lists must be sorted
  - adjacency lists must not contain duplicates
  - self loops are rejected
  - the graph must be undirected, so if `u -> v` appears then `v -> u` must also appear
- `offsets`
  - type: optional 1-D NumPy array of `np.uint64`
  - shape: `num_vertices`
  - if omitted, GraphMini computes it
  - `offsets[v]` is the split position inside vertex `v`'s adjacency list where neighbors stop being `< v`
  - equivalently:
    - `indices[indptr[v] : indptr[v] + offsets[v]]` are the neighbors smaller than `v`
    - `indices[indptr[v] + offsets[v] : indptr[v + 1]]` are the neighbors greater than or equal to `v`
  - this is used by GraphMini's bounded-neighbor and canonicality logic
- `triangles`
  - type: optional 1-D NumPy array of `np.uint64`
  - shape: `num_vertices`
  - if provided, it must follow GraphMini's raw preprocessing convention:
    - each entry is the per-vertex triangle count without canonicality constraints
    - the total sum over all vertices must therefore be divisible by `6` for an undirected graph
  - if omitted, GraphMini estimates the graph-wide triangle statistic used by scheduling:
    - it selects the top-100 highest-degree vertices
    - computes their triangle counts without canonicality constraints
    - uses the sample average to estimate the graph-wide triangle count
    - for the `graphpi` scheduler, it also uses those sampled vertices' degrees to derive the average-degree term in the cost model
  - this estimate can be larger than the true global average, but it is intentionally biased toward the high-degree region that dominates runtime
- `reorder_by_degree`
  - type: `bool`
  - default: `False`
  - applies the same degree-based graph reordering as the preprocessed-graph path

#### `compile_plan(graph, query_adjmat, query_type, pruning_type="eager", parallel_type="nested_rt", scheduler="graphpi")`

- `graph`
  - type: `pygraphmini.Graph`
  - the loaded graph object to compile against
- `query_adjmat`
  - type: `str`
  - flattened adjacency matrix for the query graph
  - length must be a perfect square
  - for a query with `k` vertices, the string length must be `k * k`
  - example:
    - triangle: `"011101110"`
    - 4-clique: `"0111101111011110"`
- `query_type`
  - type: `str`
  - required
  - allowed values:
    - `vertex`
    - `edge`
    - `edge_iep`
- `pruning_type`
  - type: `str`
  - default: `eager`
  - Python API currently supports:
    - `none`
    - `eager`
    - `costmodel`
- `parallel_type`
  - type: `str`
  - default: `nested_rt`
  - allowed values:
    - `openmp`
    - `tbb_top`
    - `nested`
    - `nested_rt`
- `scheduler`
  - type: `str`
  - default: `graphpi`
  - allowed values:
    - `graphpi`
    - `graphmini`
    - `graphzero`

#### `CompiledPlan.run(graph, num_threads=0)`

- `graph`
  - type: `pygraphmini.Graph`
  - graph to execute the compiled plan on
  - in normal usage this should be the same graph that was used during compilation
- `num_threads`
  - type: `int`
  - default: `0`
  - if `<= 0`, GraphMini uses the machine's hardware concurrency
  - otherwise it uses the requested positive thread count

### Query options

`compile_plan(...)` requires:

- `graph`
- `query_adjmat`
- `query_type`

The remaining options are optional:

- `query_type`: `vertex`, `edge`, `edge_iep`
- `pruning_type`: defaults to `eager`; Python API supports `none`, `eager`, `costmodel`
- `parallel_type`: defaults to `nested_rt`
- `scheduler`: defaults to `graphpi`

### What happens during `compile_plan(...)`

GraphMini preserves its current performance model:

1. schedule the query
2. generate specialized C++ code
3. compile that code into a shared module
4. load the module back into the current Python process

Compiled plan modules are cached under the selected build directory's
`python_plan_cache` (or a backend-specific variant), grouped by oneTBB version
and a runtime-header/build fingerprint. Repeated use of the same generated code
with the same configuration avoids recompiling.

## Example: Run on a Preprocessed Graph

Preprocess the graph once:

```bash
./build/bin/prep --path_to_graph=./dataset/wiki
```

Then use it from Python:

```python
import pygraphmini as gm

graph = gm.Graph.from_preprocessed(
    "./dataset/GraphMini/wiki",
    reorder_by_degree=True,
)

plan = gm.compile_plan(
    graph=graph,
    query_adjmat="011101110",
    query_type="vertex",
)

result = plan.run(graph, num_threads=32)

print("pattern_size:", plan.pattern_size)
print("module_path:", plan.module_path)
print("result:", result.result)
print("execution_time_seconds:", result.execution_time_seconds)
print("throughput:", result.throughput)
```

### Available graph properties

```python
print(graph.num_vertices)
print(graph.num_edges)
print(graph.num_triangles)
```

## Example: Build a Graph from NumPy CSR Arrays

`Graph.from_csr(...)` is intended for notebook use and in-memory workflows.

Requirements:

- `indptr` must be a 1-D `np.uint64` array
- `indices` must be a 1-D integer array compatible with GraphMini vertex ids
- adjacency lists must be sorted
- the graph must be undirected, so if `u -> v` appears then `v -> u` must also appear
- self loops and duplicate neighbors are rejected
- `offsets` is optional; if omitted, GraphMini computes it
- `triangles` is optional
- if `triangles` is provided, it must follow GraphMini's raw preprocessing convention: per-vertex counts without canonicality constraints, so the total sum is divisible by 6

If `triangles` is omitted, GraphMini estimates the triangle statistic needed by the scheduler:

1. select the top-100 highest-degree vertices
2. compute their triangle counts without canonicality constraints
3. use that sample average to estimate the graph-wide triangle count for schedule generation
4. if the scheduler is `graphpi`, use those same sampled vertices' degrees as the average-degree signal in its cost model

This estimate can be larger than the true graph-wide average, but the high-degree vertices dominate the expensive parts of the search, so their statistics are used to drive scheduling.

Example:

```python
import numpy as np
import pygraphmini as gm

indptr = np.array([0, 2, 4, 6], dtype=np.uint64)
indices = np.array([1, 2, 0, 2, 0, 1], dtype=np.uint32)

graph = gm.Graph.from_csr(
    indptr=indptr,
    indices=indices,
    offsets=None,
    triangles=None,
    reorder_by_degree=False,
)

plan = gm.compile_plan(
    graph=graph,
    query_adjmat="011101110",
    query_type="vertex",
)

result = plan.run(graph, num_threads=1)
print(result.result)
```

## Example: Reuse a Compiled Plan

If you want to compile once and run multiple times:

```python
import pygraphmini as gm

graph = gm.Graph.from_preprocessed("./dataset/GraphMini/wiki")
plan = gm.CompiledPlan(
    graph=graph,
    query_adjmat="011101110",
    query_type="vertex",
)

for threads in (1, 8, 32):
    result = plan.run(graph, num_threads=threads)
    print(threads, result.execution_time_seconds, result.result)
```

## `RunResult` Fields

`plan.run(...)` returns a `RunResult` object with:

- `number_of_matches` (`result` remains an alias)
- `execution_time_seconds`
- `throughput`
- `num_threads`
- `vertex_allocated`
- `minigraph_allocated`
- `thread_min_time_seconds`
- `thread_mean_time_seconds`
- `thread_max_time_seconds`
- `thread_time_std_seconds`
