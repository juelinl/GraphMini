"""Integration checks with adjacency lists large enough to enter SIMD kernels."""
import itertools
import math
import numpy as np
import pygraphmini as gm
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
n = 64
indices = np.array([j for i in range(n) for j in range(n) if i != j], dtype=np.uint32)
indptr = np.arange(n + 1, dtype=np.uint64) * (n - 1)
graph = gm.Graph.from_csr(indptr, indices)
queries = [
    (matrix(3, [(0, 1), (1, 2), (0, 2)]), "vertex", math.comb(n, 3)),
    (matrix(4, [(0, 1), (0, 2), (0, 3)]), "edge_iep", n * math.comb(n - 1, 3)),
    (matrix(4, [(0, 1), (0, 2), (0, 3)]), "vertex", 0),
]
cases = 0
for (query, mode, expected), pruning, parallel in itertools.product(
    queries, ["none", "eager", "costmodel"], ["openmp", "nested_rt"]
):
    plan = gm.compile_plan(
        graph, "".join(str(x) for row in query for x in row), mode,
        pruning_type=pruning, parallel_type=parallel, scheduler="graphpi"
    )
    for threads in (1, 2):
        actual = plan.run(graph, num_threads=threads).number_of_matches
        assert actual == expected, (mode, pruning, parallel, actual, expected)
        cases += 1
print(f"Validated {cases} large-adjacency executions against complete-graph formulas", flush=True)
