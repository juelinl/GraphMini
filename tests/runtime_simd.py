"""Integration checks with adjacency lists large enough to enter SIMD kernels."""
import itertools
import math
import random
import numpy as np
import graphmini as gm
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
n = 64
rng = random.Random(20260907)
edges = list(itertools.combinations(range(n), 2))
graphs = [matrix(n, edges), matrix(n, [e for e in edges if rng.random() < 0.7])]
cases = 0
# Largest degree first: pooled workspaces need not grow between these graphs.
for data in graphs:
    neighbors = [[j for j in range(n) if data[i][j]] for i in range(n)]
    indices = np.array([j for row in neighbors for j in row], dtype=np.uint32)
    indptr = np.array([0, *itertools.accumulate(map(len, neighbors))], dtype=np.uint64)
    graph = gm.Graph.from_csr(indptr, indices)
    # Count each triangle or center/unordered-leaf triple exactly once:
    # no compiler schedule or symmetry correction is used in the oracle.
    triangles = sum(data[i][j] and data[i][k] and data[j][k]
                    for i, j, k in itertools.combinations(range(n), 3))
    edge_stars = sum(math.comb(len(row), 3) for row in neighbors)
    induced_stars = sum(not (data[i][j] or data[i][k] or data[j][k])
                        for row in neighbors for i, j, k in itertools.combinations(row, 3))
    queries = [
        (matrix(3, [(0, 1), (1, 2), (0, 2)]), "vertex", triangles),
        (matrix(4, [(0, 1), (0, 2), (0, 3)]), "edge_iep", edge_stars),
        (matrix(4, [(0, 1), (0, 2), (0, 3)]), "vertex", induced_stars),
    ]
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
print(f"Validated {cases} large-adjacency executions against symmetry-free combinatorial counts",
      flush=True)
