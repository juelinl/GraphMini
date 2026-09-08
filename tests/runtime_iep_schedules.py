"""Check changed IEP schedules using distinct image-edge sets, without symmetry division."""
import argparse
import json
import random

import graphmini as gm
import numpy as np
from runtime_test_support import restore_generated_plan_at_exit


def count_edge_images(host, query):
    # Automorphic embeddings produce exactly the same set of image edges.
    # Unlike induced matching, a vertex subset may contain several edge images.
    edges = [(i, j) for i in range(len(query)) for j in range(i) if query[i][j]]
    images = set()
    assigned = []
    used = set()

    def search(i):
        if i == len(query):
            images.add(tuple(sorted((min(assigned[a], assigned[b]), max(assigned[a], assigned[b]))
                                    for a, b in edges)))
            return
        for vertex in range(len(host)):
            if vertex in used or any(query[i][j] and not host[vertex][assigned[j]] for j in range(i)):
                continue
            assigned.append(vertex)
            used.add(vertex)
            search(i+1)
            used.remove(vertex)
            assigned.pop()

    search(0)
    return len(images)


def graph(data):
    offsets, ids = [0], []
    for row in data:
        ids.extend(i for i, edge in enumerate(row) if edge)
        offsets.append(len(ids))
    return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64), np.array(ids, dtype=np.uint32))


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("comparison")
    args = parser.parse_args()
    with open(args.comparison) as source:
        records = json.load(source)["patterns"]
    selected = {}
    for n in (6, 7):
        group = [r for r in records if r["n"] == n]
        for baseline in (0, 1):
            gains = [r for r in group if r["plans"][2]["iep_width"] > r["plans"][baseline]["iep_width"]]
            if gains:
                for r in (min(gains, key=lambda r: r["edges"]), max(gains, key=lambda r: r["edges"]),
                          max(gains, key=lambda r: r["plans"][2]["iep_width"] - r["plans"][baseline]["iep_width"])):
                    selected[r["atlas_id"]] = r
        # Maximum-width star and no-IEP complete graph controls.
        for r in (max(group, key=lambda r: r["optimum_width"]), max(group, key=lambda r: r["edges"])):
            selected[r["atlas_id"]] = r
    rng = random.Random(923)
    executions = 0
    for atlas_id, r in selected.items():
        n, bits = r["n"], r["input_adjacency"]
        query = [[int(bits[i*n+j]) for j in range(n)] for i in range(n)]
        planted = [[0] * (n+1) for _ in range(n+1)]
        for i in range(n+1):
            for j in range(i+1, n+1):
                planted[i][j] = planted[j][i] = query[i][j] if j < n else int(rng.random() < .5)
        labels = list(range(n+1))
        rng.shuffle(labels)
        hosts = [planted, [[planted[i][j] for j in labels] for i in labels]]
        for density in (.35, .85):
            data = [[0] * (n+1) for _ in range(n+1)]
            for i in range(n+1):
                for j in range(i+1, n+1):
                    data[i][j] = data[j][i] = int(rng.random() < density)
            hosts.append(data)
        expected = [count_edge_images(data, query) for data in hosts]
        assert expected[0] >= 1 and expected[0] == expected[1]
        graphs = [graph(data) for data in hosts]
        for scheduler in ("graphmini", "outgoing", "iep_first"):
            for mode in ("edge", "edge_iep"):
                plan = gm.compile_plan(graphs[0], bits, mode, scheduler=scheduler,
                                       pruning_type="none", parallel_type="openmp")
                for host, count in zip(graphs, expected):
                    for threads in (1, 2):
                        actual = plan.run(host, num_threads=threads).number_of_matches
                        assert actual == count, (atlas_id, scheduler, mode, actual, count)
                        executions += 1
        print(f"Validated atlas {atlas_id}, n={n}, edges={r['edges']}, "
              f"widths={[p['iep_width'] for p in r['plans']]}", flush=True)
    print(f"Validated {executions} executions on {len(selected)} patterns against image-edge oracle", flush=True)


if __name__ == "__main__":
    main()
