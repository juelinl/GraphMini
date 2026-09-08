"""Positive generated-query test with an anchor whose BitGraph exceeds its budget."""
import ctypes
import argparse
import itertools
import graphmini as gm
import numpy as np
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
parser = argparse.ArgumentParser()
parser.add_argument("--parallel", default="openmp")
args = parser.parse_args()
degree = 20000 # Full universe rows alone exceed the 32 MiB region budget.
adjacency = [[] for _ in range(degree + 1)]
core = list(range(degree - 3, degree + 1))
edges = list(itertools.combinations(core, 2))
edges.extend((leaf, degree) for leaf in range(degree - 3))
for a, b in edges:
    adjacency[a].append(b)
    adjacency[b].append(a)
offsets, indices = [0], []
for row in adjacency:
    indices.extend(sorted(row))
    offsets.append(len(indices))
host = gm.Graph.from_csr(np.array(offsets, dtype=np.uint64), np.array(indices, dtype=np.uint32))
# Only the four core vertices have degree >=3, so there is exactly one K4.
plans = [gm.compile_plan(host, "0111101111011110", "vertex", scheduler="graphmini",
                         pruning_type="none", parallel_type=args.parallel, bitmap=bitmap,
                         bitmap_diagnostics=bitmap) for bitmap in (False, True)]
assert "// bitmap local-index loop" in plans[1].generated_code
for threads in (1, 2):
    for plan in plans:
        assert plan.run(host, num_threads=threads).number_of_matches == 1
    library = ctypes.CDLL(plans[1].module_path)
    counter = library.graphmini_bitmap_counter
    counter.argtypes = [ctypes.c_uint]
    counter.restype = ctypes.c_uint64
    # The sole match is rooted at the highest-ID center; its region is rejected.
    assert counter(5) > 0, "Array fallback was not executed"
    assert counter(6) == 1, "Expected the positive count to use the array fallback"
print("Validated 4 positive generated executions with budget-rejected bitmap region", flush=True)
