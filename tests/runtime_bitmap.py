"""Induced clique-like corpus; independent subset oracle, then array/bitmap comparison."""
import argparse
import ctypes
import itertools
import random

import graphmini as gm
import numpy as np

from induced_subset_oracle import count_induced_subsets
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
parser = argparse.ArgumentParser()
parser.add_argument("--bitmap", action="store_true")
parser.add_argument("--scheduler", default="graphpi", choices=["graphpi", "graphmini", "graphzero"])
parser.add_argument("--relabel-pattern", action="store_true")
args = parser.parse_args()
rng = random.Random(917)
cases = 0
bitmap_calls = 0
bitmap_builds = 0
bitmap_rows = 0
bitmap_conversions = 0
observed_reuse = False
for size in range(4, 8):
    edges = list(itertools.combinations(range(size), 2))
    for missing in [[], [(0, 1)], [(0, 1), (0, 2)], [(0, 1), (2, 3)]]:
        query = matrix(size, [e for e in edges if e not in missing])
        if args.relabel_pattern:
            order = list(range(size))
            rng.shuffle(order)
            query = [[query[i][j] for j in order] for i in order]
        # Planted induced pattern plus isolates and a relabeled copy of that host.
        planted = matrix(size + 1, [e for e in edges if e not in missing])
        permutation = list(range(size + 1))
        rng.shuffle(permutation)
        relabeled = [[planted[i][j] for j in permutation] for i in permutation]
        host_edges = list(itertools.combinations(range(size + 1), 2))
        hosts = [planted, relabeled, matrix(size + 1, host_edges),
                 matrix(size + 1, []),
                 matrix(size + 1, [e for e in host_edges if rng.random() < .75])]
        hosts.extend(matrix(size + 1, [e for e in host_edges if rng.random() < density])
                     for density in [.25, .5, .7, .85, .95])
        if size == 4:
            hosts.extend(matrix(4, [edge for bit, edge in enumerate(edges) if mask & (1 << bit)])
                         for mask in range(1 << len(edges)))

        def graph(data):
            indices, offsets = [], [0]
            for row in data:
                indices.extend(i for i, edge in enumerate(row) if edge)
                offsets.append(len(indices))
            return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64),
                                     np.array(indices, dtype=np.uint32))

        graphs = [graph(data) for data in hosts]
        pattern = "".join(str(x) for row in query for x in row)
        plans = [gm.compile_plan(graphs[0], pattern, "vertex", pruning_type="none",
                                 parallel_type="openmp", scheduler=args.scheduler)]
        if args.bitmap:
            plans.append(gm.compile_plan(graphs[0], pattern, "vertex", pruning_type="none",
                                         parallel_type="openmp", scheduler=args.scheduler,
                                         bitmap=True, bitmap_diagnostics=True))
        for data, host in zip(hosts, graphs):
            expected = count_induced_subsets(data, query)
            for plan in plans:
                for threads in [1, 2]:
                    result = plan.run(host, num_threads=threads)
                    actual = result.number_of_matches
                    assert actual == expected, (size, missing, threads, actual, expected, data)
                    cases += 1
                    if args.bitmap and plan is plans[-1]:
                        library = ctypes.CDLL(plan.module_path)
                        counter = library.graphmini_bitmap_counter
                        counter.argtypes = [ctypes.c_uint]
                        counter.restype = ctypes.c_uint64
                        bitmap_builds += counter(0)
                        bitmap_rows += counter(1)
                        bitmap_conversions += counter(2)
                        bitmap_calls += counter(3)
                        observed_reuse |= counter(2) > counter(0) and counter(3) > 0
                        assert counter(4) == threads, "OpenMP team does not match requested threads"
                        if expected and "// bitmap-region build once" in plan.generated_code:
                            assert counter(0) > 0 and counter(3) > 0, "Eligible positive case used no bitmaps"
                            assert counter(1) >= counter(0), "Constructed an empty BitGraph"
        print(f"Validated K{size} minus {missing}", flush=True)
print(f"Validated {cases} induced clique-like executions against subset oracle", flush=True)
if args.bitmap:
    assert bitmap_calls > 0 and bitmap_builds > 0, "Bitmap route silently fell back everywhere"
    assert observed_reuse, "No case reused a BitGraph across prefix bindings"
    print(f"Bitmap builds={bitmap_builds}, rows={bitmap_rows}, conversions={bitmap_conversions}, "
          f"count calls={bitmap_calls}", flush=True)
