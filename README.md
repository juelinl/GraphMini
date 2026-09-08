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

### Alternative: Conda environment

The repository includes `environment.yml` with Python 3.14 and current compatible
releases of NumPy, CMake, Ninja, the C++ compiler, clang-format, OpenMP, oneTBB,
fmt, cxxopts, and pybind11. On macOS, Xcode Command Line Tools provide the SDK.
CMake discovers the installed packages; it does not fetch pinned dependency copies.

```bash
conda env create -f environment.yml
conda activate graphmini
python scripts/install_onetbb.py
cmake -S . -B build-conda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python" -DOpenMP_ROOT="$CONDA_PREFIX" \
  -DGRAPHMINI_BUILD_TESTS=ON
python scripts/install_python.py --build-dir build-conda
cmake --build build-conda --target compiler_regression
ctest --test-dir build-conda --output-on-failure
```

Micromamba users can create the same environment with
`micromamba create -f environment.yml` and activate it with
`micromamba activate graphmini`. Use this setup instead of the virtual-environment
steps below. `build-conda` keeps its Python-specific build separate from `build`.

GraphMini requires **oneTBB 2023.1 or newer**. The installer above builds the
2023.1.0 tag into `.deps/oneTBB-2023.1.0`, including its preview `oneapi/tbb.cppm`
module interface. This is needed when Conda's platform packages are older; it
does not overwrite them. CMake prefers this local install. If your package
manager already provides a suitable version, the installer can be skipped.
The default still uses headers/PCH: installing the release does not itself
enable the official named module. Finished query-library caches are separated
by oneTBB version so an upgrade does not reuse old query binaries.

On the tested Clang 21/libc++ setup, upstream 2023.1.0's unmodified `tbb.cppm`
currently fails to compile: it unconditionally exports `cache_aligned_resource`
and `scalable_memory_resource`, while oneTBB's feature check disables those
declarations for libc++. An opt-in build-local workaround is available:

```bash
cmake -S . -B build-tbb-module -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHMINI_BUILD_TESTS=ON -DGRAPHMINI_EXPERIMENTAL_TBB_MODULE=ON
cmake --build build-tbb-module --target tbb_module_smoke
ctest --test-dir build-tbb-module -R '^tbb_module_smoke$' --output-on-failure
```

This requires CMake 3.28+ and upstream Clang/Ninja. CMake generates a copy at
`build-tbb-module/generated/tbb-module/tbb.cppm`, adding the existing feature
guard around just those two exports for 2023.1.0. Installed and vendor sources
remain untouched. The `graphmini_tbb_module` target exposes the named `tbb`
module; its smoke test imports it and exercises parallel execution and both
allocator types. With this option enabled, dynamic query plans also use
`import tbb` through the backend header. Build `pygraphmini` and `plan_module`
in this directory and set `PYTHONPATH=build-tbb-module/lib` to test that path.
The default, static plans, and profiling plans retain PCH. The named-module
and header-unit options are mutually exclusive.

To precompile GraphMini's backend as well, use the experimental backend module:

```bash
cmake -S . -B build-backend-module -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHMINI_BUILD_TESTS=ON -DGRAPHMINI_EXPERIMENTAL_BACKEND_MODULE=ON
cmake --build build-backend-module --parallel 6
ctest --test-dir build-backend-module --output-on-failure
PYTHONPATH=build-backend-module/lib python tests/runtime_large.py
```

This enables the oneTBB module automatically. Dynamic queries import
`graphmini.backend`, which exports the stable runtime types and re-exports `tbb`.
The interface preserves the header-defined types' identity for interoperability
with the C++17 Python host. Templates/inline methods remain available to the
optimizer; this is not a conversion of static/profiling plans or `std` to modules.
PCH remains the default. Use a separate build directory for each experimental
backend; finished query libraries also have backend-specific caches.

For a serial PCH/backend-module correctness and compilation/API comparison:

```bash
python scripts/verify_platform.py --backend-module \
  --module-build build-backend-module --output-dir .verification/backend
```

On Ubuntu, install upstream Clang and matching `clang-scan-deps` (for example,
Conda's `clangxx=21` and `clang-tools=21`) and add `--compiler clang++`.
See the [backend-module experiment results](tests/benchmarks/backend-module-verification.md)
for correctness coverage, compilation timings, API timings, and limitations.
For the stage-level GraphPi/Ninja/Clang breakdown and profiling commands, see
[compilation profiling](tests/benchmarks/compilation-profile.md).
The optional `GRAPHMINI_EXPERIMENTAL_NO_INLINE=ON` diagnostic keeps `-O3` but
adds `-fno-inline` to dynamic queries. It is off by default; see the
[compilation/execution tradeoff](tests/benchmarks/no-inline-experiment.md)
before using it. It is not a selective inner-loop inlining policy.

Query-library caches include a fingerprint of runtime headers and compiler/build
settings, so runtime fixes do not silently reuse stale generated libraries.

### 1. Create a Python environment

For a manual setup, first install oneTBB (including development headers and
tbbmalloc), fmt, cxxopts, pybind11's CMake package, OpenMP, and a C++17 compiler.
Set `CMAKE_PREFIX_PATH` to their installation prefix if needed. The Conda setup
above installs these dependencies together.

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
4. macOS arm64: builds and small-graph runtime checks verified with the Conda environment
5. native Windows: portability changes have been added, but this remains untested

Platform outlook:

- Linux support is already in place.
- macOS arm64 has been verified with compiler regression and runtime compilation tests.
- native Windows is more plausible now because shared-module loading, process helpers, and graph memory mapping no longer assume POSIX-only APIs.

Remaining caveats:

- native Windows builds remain untested
- the dataset helper scripts are shell scripts aimed at Unix-like environments
- macOS testing covers small graphs, not the full benchmark datasets

For the PCH/named-module refactor checks and six-/seven-vertex compilation
measurements on macOS and Ubuntu, see
[verification results](tests/benchmarks/named-module-verification.md).
Run `python scripts/verify_platform.py` in the build environment to reproduce
both backend suites; on Ubuntu with upstream Clang installed, add
`--compiler clang++`. Run serially per source checkout because runtime code
generation shares `src/codegen_output/plan.cpp`.

### SIMD set operations

The normal runtime uses header-only NEON (ARM64) or CPU-checked AVX2 (GCC/Clang
x86) intersection and subtraction kernels for sufficiently large sorted candidate
sets, with scalar fallback. Count-only and output-producing operations share
the comparison logic. Profiling retains the original scalar-work counters.
See [implementation and validation](tests/benchmarks/simd-set-operations.md)
for memory contracts, tests, and benchmark limitations.
The [subtraction follow-up](tests/benchmarks/simd-subtraction.md) covers the
accumulated match masks, upper bounds, and extra vertex exclusion.

Search, removal, and index mapping also use shared array kernels. Index mapping
supports NEON/AVX2; small-set SIMD search remains a benchmark candidate.
See [sorted-set kernels and measurements](tests/benchmarks/sorted-set-operations.md).

### Vertex-set workspace pools

Normal and profiling VertexSet share a dedicated header-only, fixed-capacity
worker pool. Compatible requests reuse buffers; larger requests cannot reuse
undersized buffers from earlier graphs. Owning sets retain their originating
pool through moves and temporary views. Generated entry points configure the
pool before starting workers; allocation accounting also belongs to the pool.
See [pool design and verification](tests/benchmarks/vertex-set-pool.md), including
thread-confinement and cache-retention limitations.
VertexSet uses two pointers and two 32-bit fields (24 bytes on the supported
64-bit targets); sizes beyond UINT32_MAX are rejected rather than truncated.

MiniGraph's normal and profiling backends also share a variable-capacity pool
and move-only scratch container. See [MiniGraph storage design and tests](tests/benchmarks/minigraph-pool.md).

### Tooling

1. CMake >= 3.20
2. A C++17 compiler (the Conda environment supplies the current platform compiler)
3. Python 3.14 in the supplied environment
4. `numpy` for the Python CSR API
5. `clang-format` optional
6. Installed CMake packages for oneTBB, fmt, cxxopts, and pybind11, plus OpenMP

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
