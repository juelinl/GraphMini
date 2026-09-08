"""Full execution comparison. No diagnostic counters; compilation timed separately."""
import itertools
import json
import random
import statistics
import graphmini as gm
import numpy as np
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
results = []
for size, host_size in [(4, 65), (5, 40), (6, 24), (7, 20)]:
    for missing in [False, True]:
        query = matrix(size, [e for e in itertools.combinations(range(size), 2)
                              if not missing or e != (0, 1)])
        for density in [.25, .8]:
            rng = random.Random(919)
            data = matrix(host_size, [e for e in itertools.combinations(range(host_size), 2)
                                      if rng.random() < density])
            ids, offsets = [], [0]
            for row in data:
                ids.extend(i for i, edge in enumerate(row) if edge)
                offsets.append(len(ids))
            graph = gm.Graph.from_csr(np.array(offsets, dtype=np.uint64),
                                      np.array(ids, dtype=np.uint32))
            pattern = "".join(str(v) for row in query for v in row)
            plans = [gm.compile_plan(graph, pattern, "vertex", pruning_type="none",
                                     parallel_type="openmp", bitmap=bitmap)
                     for bitmap in [False, True]]
            times = [[], []]
            expected = plans[0].run(graph, num_threads=1).number_of_matches
            assert plans[1].run(graph, num_threads=1).number_of_matches == expected
            for trial in range(7):
                for index in ([0, 1] if trial % 2 == 0 else [1, 0]):
                    result = plans[index].run(graph, num_threads=1)
                    assert result.number_of_matches == expected
                    times[index].append(result.execution_time_seconds)
            medians = [statistics.median(t) for t in times]
            row = dict(size=size, missing_edge=missing, vertices=host_size, density=density,
                       count=expected, selected="// bitmap-region build once" in plans[1].generated_code,
                       array_seconds=medians[0], bitmap_seconds=medians[1],
                       speedup=medians[0] / medians[1], samples=times,
                       compilation=[p.compilation_profile for p in plans])
            results.append(row)
            print("BITMAP_RESULT " + json.dumps(row), flush=True)
