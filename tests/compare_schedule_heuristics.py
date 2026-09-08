"""Compare all connected unlabeled 6/7-vertex Graph Atlas patterns (requires networkx).

Run: python tests/compare_schedule_heuristics.py --executable build-macos-clang/bin/schedule_experiment
Outputs raw records and aggregate JSON; this is a plan experiment, not a runtime benchmark.
"""
import argparse
from collections import Counter
import itertools
import json
from pathlib import Path
import random
import subprocess

import networkx as nx

NAMES = {1: "graphmini", 3: "outgoing", 4: "bitmap_balanced"}


def adjacency(graph):
    return "".join(str(int(graph.has_edge(i, j))) for i in range(len(graph)) for j in range(len(graph)))


def graph_from_string(bits, n):
    graph = nx.empty_graph(n)
    graph.add_edges_from((i, j) for i in range(n) for j in range(i + 1, n) if bits[i*n+j] == "1")
    return graph


def closure(pairs, n):
    relation = [[False] * n for _ in range(n)]
    for a, b in pairs:
        relation[a][b] = True
    for k in range(n):
        for i in range(n):
            for j in range(n):
                relation[i][j] |= relation[i][k] and relation[k][j]
    assert not any(relation[i][i] for i in range(n)), "Cyclic canonicality constraints"
    return "".join(str(int(value)) for row in relation for value in row)


def verify_symmetry(record, host):
    """Count accepted isomorphisms directly; no scheduler's symmetry divisor."""
    query = graph_from_string(record["adjacency"], len(host))
    accepted = sum(all(mapping[a] > mapping[b] for a, b in record["restrictions"])
                   for mapping in nx.isomorphism.GraphMatcher(query, host).isomorphisms_iter())
    assert accepted == 1, (record, "Expected one representative", accepted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", default="/tmp/graphmini-schedule-comparison.json")
    parser.add_argument("--sizes", type=int, nargs="+", choices=range(2, 8), default=[6, 7])
    args = parser.parse_args()
    corpus = [(index, g) for index, g in enumerate(nx.graph_atlas_g())
              if len(g) in args.sizes and nx.is_connected(g)]
    totals = Counter(len(g) for _, g in corpus)
    for n in args.sizes:
        if n in (6, 7):
            assert totals[n] == {6: 112, 7: 853}[n]
    # Repeat reversed input labels: structural results must be label-invariant.
    inputs = []
    for _, graph in corpus:
        inputs.extend([f"{len(graph)} {adjacency(graph)}",
                       f"{len(graph)} {adjacency(nx.relabel_nodes(graph, {i: len(graph)-1-i for i in graph}))}"])
    result = subprocess.run([str(Path(args.executable).resolve())], input="\n".join(inputs) + "\n",
                            text=True, capture_output=True, check=True)
    # Compiler diagnostics also go to stdout; protocol rows are tab-separated.
    rows = [line.split("\t") for line in result.stdout.splitlines()
            if line.startswith(("1\t", "3\t", "4\t"))]
    assert len(rows) == len(corpus) * 6, (len(rows), result.stderr)
    records = []
    rng = random.Random(920)
    for index, (atlas_id, graph) in enumerate(corpus):
        plans = []
        for fields in rows[index*6:(index+1)*6]:
            policy, bits, order, pairs, opportunity, entry, anchor, reason = fields
            pairs = [list(map(int, pair.split(":"))) for pair in pairs.split(",") if pair]
            plan = dict(scheduler=NAMES[int(policy)], adjacency=bits,
                        order=[int(v) for v in order.split(",") if v], restrictions=pairs,
                        closure=closure(pairs, len(graph)), opportunity_entry=int(opportunity),
                        bitmap_entry=int(entry), bitmap_anchor=int(anchor), bitmap_reason=reason)
            plans.append(plan)
        for a, b in zip(plans[:3], plans[3:]):
            assert all(a[key] == b[key] for key in a if key != "order"), (atlas_id, "Label-dependent result")
        labels = list(graph)
        rng.shuffle(labels)
        relabeled = nx.relabel_nodes(graph, dict(enumerate(labels)))
        for plan in plans[:3]:
            verify_symmetry(plan, graph)
            verify_symmetry(plan, relabeled)
        records.append(dict(atlas_id=atlas_id, n=len(graph), edges=graph.number_of_edges(),
                            graph6=nx.to_graph6_bytes(graph, header=False).decode().strip(),
                            input_adjacency=adjacency(graph), plans=plans[:3]))
        if (index + 1) % 100 == 0:
            print(f"Verified {index+1}/{len(corpus)} patterns", flush=True)
    summary = {}
    for n in args.sizes:
        group = [r for r in records if r["n"] == n]
        comparisons = {}
        for i, j in itertools.combinations(range(3), 2):
            stats = Counter()
            for record in group:
                a, b = record["plans"][i], record["plans"][j]
                structure = a["adjacency"] == b["adjacency"]
                full = structure and a["closure"] == b["closure"]
                stats["structure_same" if structure else "structure_different"] += 1
                stats["schedule_same" if full else "schedule_different"] += 1
                # -1 is fallback, ranked later than any supported entry.
                x, y = [p["bitmap_entry"] if p["bitmap_entry"] >= 0 else n for p in (a, b)]
                stats["bitmap_earlier" if y < x else "bitmap_later" if y > x else "bitmap_same"] += 1
            comparisons[f"{NAMES[[1,3,4][i]]}_vs_{NAMES[[1,3,4][j]]}"] = dict(stats)
        summary[n] = dict(patterns=len(group), comparisons=comparisons,
                          bitmap_entries={name: dict(Counter(r["plans"][i]["bitmap_entry"] for r in group))
                                          for i, name in enumerate(NAMES.values())})
    output = dict(networkx_version=nx.__version__, shortlist=8, summary=summary, patterns=records)
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
