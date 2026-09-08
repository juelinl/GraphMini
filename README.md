# GraphMini

GraphMini is a high-performance graph pattern-matching system that generates
specialized C++ code for your query. Use it from Python or through the command line.

## Installation

The recommended setup uses Conda on Linux or macOS. On macOS, install Xcode
Command Line Tools first (`xcode-select --install`).

```bash
git clone https://github.com/juelinl/GraphMini.git
cd GraphMini

conda env create -f environment.yml
conda activate graphmini

python scripts/install_onetbb.py
cmake -S . -B build-conda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python" -DOpenMP_ROOT="$CONDA_PREFIX"
python scripts/install_python.py --build-dir build-conda
```

The environment provides Python 3.14 and the build dependencies. The oneTBB
installer supplies version 2023.1.0 locally; skip it if you already have
oneTBB 2023.1 or newer.

This is a source-tree installation: keep the repository, build directory, and
compiler available for runtime query compilation.
For manual installation, Micromamba, uninstalling, or experimental C++20 modules,
see the [setup guide](docs/setup.md).

## Your first query

This self-contained example counts triangles in a three-vertex graph:

```python
import numpy as np
import pygraphmini as gm

# Undirected triangle, stored as CSR adjacency lists.
graph = gm.Graph.from_csr(
    indptr=np.array([0, 2, 4, 6], dtype=np.uint64),
    indices=np.array([1, 2, 0, 2, 0, 1], dtype=np.uint32),
)

plan = gm.compile_plan(
    graph,
    query_adjmat="011101110",  # Flattened 3 x 3 adjacency matrix.
    query_type="vertex",
)
result = plan.run(graph, num_threads=1)
print(result.number_of_matches)  # 1
```

You can reuse `plan` for repeated runs without compiling it again, or load an
existing dataset with `gm.Graph.from_preprocessed("path/to/graph")`.

CSR input must describe a simple undirected graph: sorted, duplicate-free
adjacency lists, valid vertex IDs, and no self-loops. Use `uint64` row pointers
and `uint32` vertex IDs.

## Query options

- Query types: `vertex`, `edge`, `edge_iep`.
- Pruning: `eager` (default), `none`, `costmodel`.
- Scheduler: `graphpi` (default), `graphmini`, `graphzero`.
- Parallel execution: `nested_rt` (default), `openmp`, `tbb_top`, `nested`.

The first compilation schedules the query, generates C++, and builds a shared
library. Compatible compiled libraries are cached for reuse. Generate plans
serially per checkout; independent concurrent queries are not currently supported.

See the [Python API reference](docs/python-api.md) for input formats, optional
graph statistics, plan reuse, and result fields.

## Documentation

- [Command-line usage and dataset preprocessing](docs/cli.md)
- [Installation and experimental module builds](docs/setup.md)
- [Platforms, tests, SIMD kernels, and workspace pools](docs/development.md)
- [Refactoring notes](REFACTOR_PLAN.md)

Verification covers Ubuntu 22.04 and macOS ARM64; native Windows remains
untested. Memory and storage needs depend on the dataset—large benchmark
requirements are documented separately.

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
