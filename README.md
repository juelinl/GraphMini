# GraphMini

GraphMini is a high-performance graph pattern-matching system for subgraph enumeration on arbitrary patterns. It now provides both:

- a Python API for interactive use in Python scripts and Jupyter notebooks
- the original CLI for preprocessing, code generation, and benchmarking

The Python API is the easiest entry point for most users, so this README starts there.

## Table of Contents

- [Python Quick Start](#python-quick-start)
- [Python API Overview](#python-api-overview)
- [Example: Run on a Preprocessed Graph](#example-run-on-a-preprocessed-graph)
- [Example: Build a Graph from NumPy CSR Arrays](#example-build-a-graph-from-numpy-csr-arrays)
- [Example: Reuse a Compiled Plan](#example-reuse-a-compiled-plan)
- [CLI Workflow](#cli-workflow)
- [Requirements](#requirements)
- [Tested Graph Data](#tested-graph-data)
- [Citation](#citation)

## Python Quick Start

### 1. Create a Python environment

```bash
python3 -m venv venv
source venv/bin/activate
pip install numpy cmake ninja clang-format
```

### 2. Install `pygraphmini` into the environment

From the repository root:

```bash
python scripts/install_python.py
```

This is a source-tree install. The script:

1. configures CMake for the active Python interpreter
2. builds `pygraphmini` and the generated plan-module target
3. writes a `.pth` file into the environment so `import pygraphmini` resolves to this repository's build output

To remove that installation later:

```bash
python scripts/uninstall_python.py
```

You can override the interpreter or build directory if needed:

```bash
python scripts/install_python.py --python venv/bin/python --build-dir build
```

On Unix-like systems, the shell wrappers still work:

```bash
./scripts/install_python.sh
./scripts/uninstall_python.sh
```

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

Compiled plan modules are cached under `build/python_plan_cache`, so repeated use of the same generated code avoids recompiling.

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

- `result`
- `execution_time_seconds`
- `throughput`
- `num_threads`
- `vertex_allocated`
- `minigraph_allocated`
- `thread_min_time_seconds`
- `thread_mean_time_seconds`
- `thread_max_time_seconds`
- `thread_time_std_seconds`

## CLI Workflow

The CLI remains available for preprocessing and benchmark-style runs.

### Build the project

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

### Download and preprocess example datasets

```bash
bash dataset/download.sh
bash dataset/prep.sh
```

To preprocess a graph manually:

```bash
./build/bin/prep --path_to_graph=./dataset/wiki
```

### Run a single query from the CLI

```bash
./build/bin/run \
  --graph_name=wiki \
  --path_to_graph=./dataset/GraphMini/wiki \
  --query_name=P1 \
  --query_adjmat=0111101111011110 \
  --query_type=vertex \
  --pruning_type=costmodel \
  --parallel_type=nested_rt \
  --scheduler=graphmini \
  --num_threads=32 \
  --graph_reordering=true
```

Primary CLI options:

- `--graph_name`: graph nickname
- `--path_to_graph`: path to the preprocessed graph directory
- `--query_name`: query nickname
- `--query_adjmat`: flattened adjacency matrix string
- `--query_type`: `vertex`, `edge`, `edge_iep`
- `--pruning_type`: `none`, `static`, `eager`, `online`, `costmodel`
- `--parallel_type`: `openmp`, `tbb_top`, `nested`, `nested_rt`
- `--scheduler`: `graphpi`, `graphzero`, `graphmini`
- `--num_threads`: execution thread count
- `--graph_reordering`: enable or disable degree-based graph reordering

## Requirements

### Hardware

1. 128GB of free RAM to preprocess Friendster correctly
2. 180GB of free disk space to store preprocessed graphs

### Supported OS

Current status:

1. Ubuntu 22.04: supported
2. Ubuntu 24.04: supported
3. WSL on Windows: known to work
4. macOS: portability changes have been added, but this remains untested
5. native Windows: portability changes have been added, but this remains untested

Platform outlook:

- Linux support is already in place.
- macOS should be closer now because the project already had Apple-specific CMake handling, and the Python installer is no longer Linux-only.
- native Windows is more plausible now because shared-module loading, process helpers, and graph memory mapping no longer assume POSIX-only APIs.

Remaining caveats:

- macOS and Windows builds are still untested
- the dataset helper scripts are shell scripts aimed at Unix-like environments
- the build and runtime paths are still validated only on Linux in this repository

### Tooling

1. CMake >= 3.20
2. GCC >= 7
3. Python >= 3.10
4. `numpy` for the Python CSR API
5. `clang-format` optional

## Tested Graph Data

1. Wiki
2. YouTube
3. Patents
4. LiveJournal
5. Orkut
6. Friendster

## Citation

If GraphMini is helpful in your work, please consider citing the paper:

- [GraphMini: Accelerating Graph Pattern Matching Using Auxiliary Graphs](https://arxiv.org/abs/2403.01050)

```bibtex
@inproceedings{Liu_2023,
   title={GraphMini: Accelerating Graph Pattern Matching Using Auxiliary Graphs},
   url={http://dx.doi.org/10.1109/PACT58117.2023.00026},
   DOI={10.1109/pact58117.2023.00026},
   booktitle={2023 32nd International Conference on Parallel Architectures and Compilation Techniques (PACT)},
   publisher={IEEE},
   author={Liu, Juelin and Polisetty, Sandeep and Guan, Hui and Serafini, Marco},
   year={2023},
   month=oct,
   pages={211--224}
}
```
