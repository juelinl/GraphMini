"""Execute changed schedules on small hosts against the independent subset oracle.

Requires the JSON produced by compare_schedule_heuristics.py. Selects sparse and
dense examples of bitmap gains/losses as well as topology-only schedule changes.
"""
import argparse
import json
import random

import graphmini as gm
import numpy as np

from induced_subset_oracle import count_induced_subsets
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("comparison")
args = parser.parse_args()
with open(args.comparison) as source:
    records = json.load(source)["patterns"]
selected = {}
for n in (6, 7):
    for policy in (1, 2):
        for category in ("gain", "loss", "structure"):
            candidates = []
            for r in records:
                if r["n"] != n:
                    continue
                a, b = r["plans"][0], r["plans"][policy]
                x, y = [p["bitmap_entry"] if p["bitmap_entry"] >= 0 else n for p in (a, b)]
                if ((category == "gain" and y < x) or (category == "loss" and y > x)
                        or (category == "structure" and a["adjacency"] != b["adjacency"])):
                    candidates.append(r)
            if candidates:
                for r in (min(candidates, key=lambda r: r["edges"]),
                          max(candidates, key=lambda r: r["edges"])):
                    selected[r["atlas_id"]] = r


def make_graph(data):
    offsets, indices = [0], []
    for row in data:
        indices.extend(i for i, edge in enumerate(row) if edge)
        offsets.append(len(indices))
    return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64), np.array(indices, dtype=np.uint32))


rng = random.Random(921)
executions = 0
for atlas_id, record in selected.items():
    n = record["n"]
    bits = record["input_adjacency"]
    query = [[int(bits[i*n+j]) for j in range(n)] for i in range(n)]
    hosts = []
    # Positive planted query plus two vertices with random incident edges.
    planted = [[0] * (n+2) for _ in range(n+2)]
    for i in range(n+2):
        for j in range(i+1, n+2):
            planted[i][j] = planted[j][i] = query[i][j] if j < n else int(rng.random() < .5)
    hosts.append(planted)
    order = list(range(n+2))
    rng.shuffle(order)
    hosts.append([[planted[i][j] for j in order] for i in order])
    for density in (.2, .7):
        data = [[0] * (n+2) for _ in range(n+2)]
        for i in range(n+2):
            for j in range(i+1, n+2):
                data[i][j] = data[j][i] = int(rng.random() < density)
        hosts.append(data)
    graphs = [make_graph(host) for host in hosts]
    expected = [count_induced_subsets(host, query) for host in hosts]
    assert expected[0] >= 1 and expected[0] == expected[1]
    for scheduler in ("graphmini", "outgoing", "bitmap_balanced"):
        for bitmap in (False, True):
            plan = gm.compile_plan(graphs[0], bits, "vertex", scheduler=scheduler,
                                   pruning_type="none", parallel_type="openmp", bitmap=bitmap)
            for graph, count in zip(graphs, expected):
                for threads in (1, 2):
                    actual = plan.run(graph, num_threads=threads).number_of_matches
                    assert actual == count, (atlas_id, scheduler, bitmap, actual, count)
                    executions += 1
    print(f"Validated atlas {atlas_id}: n={n}, edges={record['edges']}", flush=True)
print(f"Validated {executions} executions across {len(selected)} changed-schedule patterns", flush=True)
