# GraphMini

GraphMini is a high-performance graph pattern-matching system.

## Usage

Follow the [installation guide](docs/setup.md#conda-environment), then count
triangles with the Python API:

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
