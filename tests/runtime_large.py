"""Check benchmark-sized queries against symmetry-normalized exhaustive counts.

Select the desired extension using PYTHONPATH=<build>/lib. Like compile_plan,
this test updates src/codegen_output/plan.cpp, restoring it on normal exit.
"""
import itertools
import argparse
import json
import random
import numpy as np
import graphmini as gm
from matching_oracle import count_matches, matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()

parser = argparse.ArgumentParser()
parser.add_argument("--results", help="Save counts, including any oracle mismatches")
parser.add_argument("--baseline", help="Compare all counts to another backend's result file")
args = parser.parse_args()

rng = random.Random(2026)
edges = list(itertools.combinations(range(8), 2))
data_graphs = [matrix(8, edges), matrix(8, [e for e in edges if rng.random() < 0.7])]
cases = 0
results, mismatches = [], []
for graph_id, data in enumerate(data_graphs):
    indices, indptr = [], [0]
    for row in data:
        indices.extend(i for i, edge in enumerate(row) if edge)
        indptr.append(len(indices))
    graph = gm.Graph.from_csr(np.array(indptr, dtype=np.uint64), np.array(indices, dtype=np.uint32))
    for size, family in itertools.product([6, 7], ["clique", "star", "cycle"]):
        query = matrix(size, [(i, j) for i in range(size) for j in range(i + 1, size)
                              if family == "clique" or (family == "star" and i == 0)
                              or (family == "cycle" and (j == i + 1 or (i == 0 and j == size - 1)))])
        expected = count_matches(data, query, False)
        for parallel, pruning in [("openmp", "none"), ("nested_rt", "costmodel")]:
            plan = gm.compile_plan(graph, "".join(str(x) for row in query for x in row),
                                   "edge_iep" if family == "star" else "edge",
                                   pruning_type=pruning, parallel_type=parallel, scheduler="graphpi")
            for threads in [1, 2]:
                actual = plan.run(graph, num_threads=threads).number_of_matches
                row = [graph_id, size, family, parallel, threads, actual, expected]
                results.append(row)
                if actual != expected:
                    mismatches.append(row)
                cases += 1
if args.results:
    with open(args.results, "w") as out:
        json.dump({"results": results, "oracle_mismatches": mismatches}, out, indent=2)
if args.baseline:
    with open(args.baseline) as source:
        baseline = json.load(source)
    assert results == baseline["results"], "Large-pattern counts differ from baseline"
    print(f"Validated {cases} large-pattern executions against baseline; "
          f"{len(mismatches)} oracle mismatches", flush=True)
else:
    assert not mismatches, mismatches
    print(f"Validated {cases} large-pattern executions against exhaustive counts", flush=True)
