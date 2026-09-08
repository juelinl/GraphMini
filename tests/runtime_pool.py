"""Run the same generated plans across changing graph capacities in one process."""
import itertools
import math
import numpy as np
import pygraphmini as gm
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
def complete(n):
    return gm.Graph.from_csr(
        np.arange(n + 1, dtype=np.uint64) * (n - 1),
        np.array([j for i in range(n) for j in range(n) if i != j], dtype=np.uint32))

seed = complete(4)
queries = [
    (matrix(3, [(0, 1), (0, 2), (1, 2)]), "vertex", lambda n: math.comb(n, 3)),
    (matrix(4, [(0, 1), (0, 2), (0, 3)]), "edge_iep",
     lambda n: n * math.comb(n - 1, 3)),
    (matrix(4, [(0, 1), (0, 2), (0, 3)]), "vertex", lambda n: 0),
]
plans = []
for (query, mode, expected), parallel, pruning in itertools.product(
    queries, ["openmp", "nested_rt"], ["none", "costmodel"]
):
    plan = gm.compile_plan(seed, "".join(str(x) for row in query for x in row), mode,
                           parallel_type=parallel, pruning_type=pruning, scheduler="graphpi")
    plans.append((plan, expected))
cases = 0
for n in (4, 64, 8, 128, 5, 64):
    graph = complete(n)
    for plan, expected in plans:
        for threads in (1, 2):
            actual = plan.run(graph, num_threads=threads).number_of_matches
            assert actual == expected(n), (n, threads, actual, expected(n))
            cases += 1
print(f"Validated {cases} graph-growth/shrink executions in one process", flush=True)
