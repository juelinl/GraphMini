"""Run with PYTHONPATH=<build>/lib and the build tools on PATH.

Checks compiled counts against an independent exhaustive matcher on tiny graphs.
Like compile_plan itself, this test updates src/codegen_output/plan.cpp.
"""
import itertools
import argparse
import json
import numpy as np
import pygraphmini as gm
from matching_oracle import count_matches, matrix

parser = argparse.ArgumentParser()
parser.add_argument("--results", help="Write counts and oracle mismatches to JSON")
parser.add_argument("--baseline", help="Compare counts to a previous build's JSON instead of the oracle")
args = parser.parse_args()


data = matrix(6, [(0, 1), (0, 2), (0, 3), (0, 4), (1, 2), (1, 3), (2, 3), (3, 4), (4, 5)])
indices, indptr = [], [0]
for row in data:
    indices.extend(i for i, edge in enumerate(row) if edge)
    indptr.append(len(indices))
graph = gm.Graph.from_csr(np.array(indptr, dtype=np.uint64), np.array(indices, dtype=np.uint32))
queries = [
    matrix(4, [(0, 1), (0, 2), (0, 3)]),
    matrix(4, [(0, 1), (1, 2), (2, 0), (0, 3)]),
    matrix(5, [(0, 1), (0, 2), (0, 3), (0, 4)]),  # IEP factor six
    matrix(5, [(i, j) for i in range(2) for j in range(2, 5)]),
]
cases = 0
results, mismatches = [], []
for query, mode, pruning, parallel, scheduler in itertools.product(
    queries, ["vertex", "edge", "edge_iep"], ["none", "eager", "costmodel"],
    ["openmp", "nested_rt"], ["graphpi", "graphzero", "graphmini"]
):
    plan = gm.compile_plan(graph, "".join(str(x) for row in query for x in row), mode,
                           pruning_type=pruning, parallel_type=parallel, scheduler=scheduler)
    expected = count_matches(data, query, mode == "vertex")
    for threads in [1, 2]:
        actual = plan.run(graph, num_threads=threads).number_of_matches
        results.append([queries.index(query), mode, pruning, parallel, scheduler, threads, actual, expected])
        if actual != expected:
            mismatches.append(results[-1])
        cases += 1
if args.results:
    with open(args.results, "w") as out:
        json.dump({"results": results, "oracle_mismatches": mismatches}, out, indent=2)
if args.baseline:
    with open(args.baseline) as source:
        baseline = json.load(source)
    assert results == baseline["results"], "Compiled behavior differs from baseline"
    print(f"Validated {cases} executions against baseline; {len(mismatches)} existing oracle mismatches", flush=True)
else:
    assert not mismatches, mismatches
    print(f"Validated {cases} compiled executions against exhaustive counts", flush=True)
