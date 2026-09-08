"""Small synthetic IEP scheduling timings; not a real-graph benchmark.

Counts on larger timed hosts are cross-checked against non-IEP generated plans.
Run runtime_iep_schedules.py separately for independent small-host oracle checks.
"""
import argparse
import json
from pathlib import Path
import random
import statistics

import graphmini as gm
from runtime_iep_schedules import graph
from runtime_test_support import restore_generated_plan_at_exit


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("comparison")
    parser.add_argument("--output", default=".verification/iep-schedule-timings.json")
    args = parser.parse_args()
    with open(args.comparison) as source:
        records = json.load(source)["patterns"]
    selected = {}
    for n in (6, 7):
        for baseline in (0, 1):
            group = [r for r in records if r["n"] == n and
                     r["plans"][2]["iep_width"] > r["plans"][baseline]["iep_width"]]
            if group:
                if baseline == 1:
                    # Include both known seven-vertex outgoing counterexamples.
                    for item in group:
                        selected[item["atlas_id"]] = item
                r = max(group, key=lambda r: (r["plans"][2]["iep_width"] -
                                             r["plans"][baseline]["iep_width"], r["edges"]))
                selected[r["atlas_id"]] = r
    rows = []
    for atlas_id, r in selected.items():
        for density in (.25, .7):
            rng = random.Random(924)
            size = 20
            data = [[0] * size for _ in range(size)]
            for i in range(size):
                for j in range(i+1, size):
                    data[i][j] = data[j][i] = int(rng.random() < density)
            # Plant all required edges, preserving additional host edges.
            for i in range(r["n"]):
                for j in range(r["n"]):
                    if r["input_adjacency"][i*r["n"]+j] == "1":
                        data[i][j] = 1
            host = graph(data)
            keys = [(scheduler, mode) for scheduler in ("graphmini", "outgoing", "iep_first")
                    for mode in ("edge", "edge_iep")]
            plans = [gm.compile_plan(host, r["input_adjacency"], mode, scheduler=scheduler,
                                     pruning_type="none", parallel_type="openmp")
                     for scheduler, mode in keys]
            counts = [p.run(host, num_threads=1).number_of_matches for p in plans]
            assert len(set(counts)) == 1 and counts[0] > 0, (atlas_id, counts)
            samples = [[] for _ in plans]
            for trial in range(7):
                order = list(range(len(plans)))
                random.Random(925+trial).shuffle(order)
                for index in order:
                    result = plans[index].run(host, num_threads=1)
                    assert result.number_of_matches == counts[0]
                    samples[index].append(result.execution_time_seconds)
            row = dict(atlas_id=atlas_id, n=r["n"], edges=r["edges"], host_size=size,
                       density=density, count=counts[0], widths=[p["iep_width"] for p in r["plans"]],
                       measurements=[dict(scheduler=key[0], mode=key[1],
                                          median_seconds=statistics.median(times), samples=times)
                                     for key, times in zip(keys, samples)])
            rows.append(row)
            print("IEP_TIMING " + json.dumps(row), flush=True)
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
