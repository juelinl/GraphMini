"""All connected unlabeled 6/7-vertex patterns: current, outgoing and IEP-first.

Requires NetworkX. Checks the optimum independently via connected vertex covers.
"""
import argparse
from collections import Counter
import itertools
import json
from pathlib import Path
import random
import subprocess

import networkx as nx
from compare_schedule_heuristics import adjacency, closure, verify_symmetry

NAMES = {1: "graphmini", 3: "outgoing", 5: "iep_first"}


def optimum_width(graph):
    n = len(graph)
    # Independent implementation: enumerate connected prefix sets, not orders.
    for size in range(1, n + 1):
        for prefix in itertools.combinations(graph, size):
            suffix = set(graph) - set(prefix)
            if nx.is_connected(graph.subgraph(prefix)) and graph.subgraph(suffix).number_of_edges() == 0:
                return n - size - 1 if n - size >= 3 else 0
    raise AssertionError("Connected graph has no connected vertex cover")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default=".verification/iep-schedule-comparison.json")
    args = parser.parse_args()
    corpus = [(i, g) for i, g in enumerate(nx.graph_atlas_g())
              if len(g) in (6, 7) and nx.is_connected(g)]
    assert Counter(len(g) for _, g in corpus) == {6: 112, 7: 853}
    inputs = []
    for _, g in corpus:
        inputs.extend([f"{len(g)} {adjacency(g)}",
                       f"{len(g)} {adjacency(nx.relabel_nodes(g, {i: len(g)-1-i for i in g}))}"])
    result = subprocess.run([str(Path(args.executable).resolve())], input="\n".join(inputs) + "\n",
                            capture_output=True, text=True, check=True)
    rows = [line.split("\t") for line in result.stdout.splitlines()
            if line.startswith(("1\t", "3\t", "5\t"))]
    assert len(rows) == len(corpus) * 6
    records = []
    rng = random.Random(922)
    for index, (atlas_id, graph) in enumerate(corpus):
        plans = []
        for fields in rows[index*6:(index+1)*6]:
            policy, bits, order, pairs, width, depth, terms, redundancy = fields
            order = [int(v) for v in order.split(",") if v]
            assert sorted(order) == list(range(len(graph)))
            source = graph if len(plans) < 3 else nx.relabel_nodes(graph, {i: len(graph)-1-i for i in graph})
            assert bits == "".join(str(int(source.has_edge(i, j))) for i in order for j in order)
            pairs = [list(map(int, pair.split(":"))) for pair in pairs.split(",") if pair]
            plans.append(dict(scheduler=NAMES[int(policy)], adjacency=bits, order=order,
                              restrictions=pairs, closure=closure(pairs, len(graph)), iep_width=int(width),
                              iep_depth=int(depth), terms=int(terms), redundancy=int(redundancy)))
        for a, b in zip(plans[:3], plans[3:]):
            assert all(a[key] == b[key] for key in a if key != "order"), (atlas_id, "Label dependence")
        optimum = optimum_width(graph)
        assert plans[2]["iep_width"] == optimum, (atlas_id, plans, optimum)
        assert all(p["iep_width"] <= optimum for p in plans)
        labels = list(graph)
        rng.shuffle(labels)
        for p in plans[:3]:
            verify_symmetry(p, graph)
            verify_symmetry(p, nx.relabel_nodes(graph, dict(enumerate(labels))))
        records.append(dict(atlas_id=atlas_id, n=len(graph), edges=graph.number_of_edges(),
                            input_adjacency=adjacency(graph), optimum_width=optimum, plans=plans[:3]))
        if (index+1) % 100 == 0:
            print(f"Verified {index+1}/{len(corpus)}", flush=True)
    summary = {}
    for n in (6, 7):
        group = [r for r in records if r["n"] == n]
        comparisons = {}
        for i, j in itertools.combinations(range(3), 2):
            counts = Counter()
            for r in group:
                a, b = r["plans"][i], r["plans"][j]
                x, y = a["iep_width"], b["iep_width"]
                counts["width_better" if y > x else "width_worse" if y < x else "width_same"] += 1
                same = (a["adjacency"], a["closure"]) == (b["adjacency"], b["closure"])
                counts["schedule_same" if same else "schedule_different"] += 1
            comparisons[f"{list(NAMES.values())[i]}_vs_{list(NAMES.values())[j]}"] = dict(counts)
        summary[n] = dict(patterns=len(group), comparisons=comparisons,
                          width_histograms={name: dict(Counter(r["plans"][i]["iep_width"] for r in group))
                                            for i, name in enumerate(NAMES.values())})
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(summary=summary, patterns=records), indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"Saved {path}")


if __name__ == "__main__":
    main()
