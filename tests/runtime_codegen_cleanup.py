"""Compile pruned kernels after declaration cleanup and verify their counts."""
import itertools
import math

import graphmini as gm
import numpy as np

from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()


def complete(n):
    return gm.Graph.from_csr(
        np.arange(n + 1, dtype=np.uint64) * (n - 1),
        np.array([j for i in range(n) for j in range(n) if i != j], dtype=np.uint32),
    )


checks = 0
for parallel, pruning in itertools.product(
    ("openmp", "tbb_top", "nested", "nested_rt"), ("eager", "costmodel")
):
    plan = gm.compile_plan(
        complete(8), "0111101111011110", "vertex", scheduler="graphpi",
        pruning_type=pruning, parallel_type=parallel,
    )
    code = plan.generated_code
    assert "const IdType v1 =" in code and ".bounded(v1)" in code
    assert "const IdType v2 =" not in code
    assert "using MiniGraphType =" not in code
    for n, threads in itertools.product((4, 8, 5), (1, 4)):
        actual = plan.run(complete(n), num_threads=threads).number_of_matches
        assert actual == math.comb(n, 4), (parallel, pruning, n, threads, actual)
        checks += 1
print(f"Passed {checks} pruned-codegen runtime checks", flush=True)
