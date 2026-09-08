"""Reproducible query timings; run separately in each comparison worktree.

Compilation and graph preparation are excluded. Timings include query context,
BitGraph construction, live-in conversion and counting. RSS is the cumulative
whole-process high-water mark, NOT isolated per-query bitmap memory.
"""
import argparse
import gzip
import fnmatch
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import resource
import statistics
import subprocess
import time

import graphmini as gm
import numpy as np
from induced_subset_oracle import count_induced_subsets
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit


def make_graph(rows):
    offsets, ids = [0], []
    for row in rows:
        ids.extend(sorted(row))
        offsets.append(len(ids))
    return gm.Graph.from_csr(np.asarray(offsets, dtype=np.uint64),
                             np.asarray(ids, dtype=np.uint32))


def hosts(real_dir, selected):
    specs = [("er-sparse", 384, .04), ("er-medium", 128, .3),
             ("er-dense", 56, .8), ("clustered", 384, .7)]
    result = []
    for name, n, density in specs:
        if name not in selected:
            continue
        rng = random.Random(20260908)
        rows = [set() for _ in range(n)]
        for i in range(n):
            for j in range(i + 1, n):
                p = density if name != "clustered" or i // 32 == j // 32 else .002
                if rng.random() < p:
                    rows[i].add(j)
                    rows[j].add(i)
        result.append((name, rows, {"seed": 20260908, "within_probability": density}))
    for name, filename in [("ca-GrQc", "ca-GrQc.txt.gz"), ("wiki-Vote", "wiki-Vote.txt.gz")]:
        if name not in selected:
            continue
        path = Path(real_dir) / filename
        edges, labels = set(), set()
        with gzip.open(path, "rt") as source:
            for line in source:
                if line.startswith("#") or not line.strip():
                    continue
                a, b = map(int, line.split()[:2])
                labels.update((a, b))
                if a != b:
                    edges.add(tuple(sorted((a, b))))
        mapping = {v: i for i, v in enumerate(sorted(labels))}
        rows = [set() for _ in mapping]
        for a, b in edges:
            rows[mapping[a]].add(mapping[b])
            rows[mapping[b]].add(mapping[a])
        result.append((name, rows, {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                    "normalization": "undirected, deduplicated, no self-loops; sorted original IDs"}))
    for name, rows, meta in result:
        degrees = [len(row) for row in rows]
        meta.update(vertices=len(rows), edges=sum(degrees) // 2,
                    mean_degree=statistics.mean(degrees), max_degree=max(degrees))
        yield name, make_graph(rows), meta


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--real-dir", required=True)
    parser.add_argument("--phase", choices=["runtime", "scheduling"], default="runtime")
    parser.add_argument("--graphs", default="er-sparse,er-medium,er-dense,clustered,ca-GrQc,wiki-Vote")
    parser.add_argument("--sizes", default="4,5,6,7")
    parser.add_argument("--families", default="clique,minus1,minus2")
    parser.add_argument("--backends", default="array,bitmap", choices=["array,bitmap", "array", "bitmap"],
                        help="Single-backend runs isolate process RSS; compare counts with paired runs")
    parser.add_argument("--threads", default="1")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--round", type=int, default=0)
    parser.add_argument("--exclude", action="append", default=[],
                        help="Skip a graph:size:family glob; recorded in run metadata")
    args = parser.parse_args()
    assert args.trials >= 3
    graphs = list(hosts(args.real_dir, args.graphs.split(",")))
    metadata = dict(label=args.label, phase=args.phase, round=args.round,
                    commit=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                    module=gm.__file__, platform=platform.platform(),
                    affinity=sorted(os.sched_getaffinity(0)),
                    omp={k: os.environ.get(k) for k in ("OMP_PLACES", "OMP_PROC_BIND", "OMP_DYNAMIC")},
                    load_average=os.getloadavg(), exclusions=args.exclude,
                    backends=args.backends,
                    rss_scope="cumulative process, including graphs and Python")
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as output:
        output.write(json.dumps({"metadata": metadata}) + "\n")
        for size in map(int, args.sizes.split(",")):
            for family in args.families.split(","):
                if family not in ("clique", "minus1", "minus2", "wheel", "cycle"):
                    raise ValueError(f"Unknown pattern family: {family}")
                omitted = [] if family == "clique" else [(0, 1)]
                if family == "minus2":
                    omitted.append((2, 3))
                edges = [e for e in itertools.combinations(range(size), 2) if e not in omitted]
                if family == "cycle":
                    edges = [(i, (i + 1) % size) for i in range(size)]
                elif family == "wheel":
                    edges = [(0, i) for i in range(1, size)] + [(i, i + 1) for i in range(1, size - 1)] + [(1, size - 1)]
                query = matrix(size, edges)
                pattern = "".join(str(v) for row in query for v in row)
                rng = random.Random(20260909)
                calibration = matrix(10, [e for e in itertools.combinations(range(10), 2)
                                          if rng.random() < .6])
                for i in range(size):
                    for j in range(size):
                        calibration[i][j] = query[i][j]
                calibration_graph = make_graph([{j for j, bit in enumerate(row) if bit} for row in calibration])
                expected = count_induced_subsets(calibration, query)
                keys = [("outgoing", flag == "bitmap") for flag in args.backends.split(",")] if args.phase == "runtime" else [
                    (scheduler, True) for scheduler in ("graphmini", "outgoing", "bitmap_balanced")]
                plans = [gm.compile_plan(calibration_graph, pattern, "vertex", scheduler=scheduler,
                                         pruning_type="none", parallel_type="openmp", bitmap=bitmap)
                         for scheduler, bitmap in keys]
                assert expected > 0
                for plan in plans:
                    assert plan.run(calibration_graph, num_threads=1).number_of_matches == expected
                code_dir = path.parent / (path.stem + "-plans")
                code_dir.mkdir(exist_ok=True)
                for (scheduler, bitmap), plan in zip(keys, plans):
                    (code_dir / f"{size}-{family}-{scheduler}-{int(bitmap)}.cpp").write_text(plan.generated_code)
                for graph_name, graph, graph_meta in graphs:
                    if any(fnmatch.fnmatchcase(f"{graph_name}:{size}:{family}", glob) for glob in args.exclude):
                        continue
                    for threads in map(int, args.threads.split(",")):
                        warmed = [p.run(graph, num_threads=threads) for p in plans]
                        counts = [r.number_of_matches for r in warmed]
                        assert len(set(counts)) == 1, (size, family, graph_name, counts)
                        repeats = [min(100, max(1, math.ceil(.02 / max(r.execution_time_seconds, 1e-9))))
                                   for r in warmed]
                        samples = [[] for _ in plans]
                        walls = [[] for _ in plans]
                        # Bound development-run cost on unexpectedly expensive cases.
                        trials = 3 if max(r.execution_time_seconds for r in warmed) > 2 else args.trials
                        for trial in range(trials):
                            order = list(range(len(plans)))
                            random.Random(20260910 + args.round * 100 + trial).shuffle(order)
                            for index in order:
                                start = time.perf_counter()
                                total = 0.0
                                for _ in range(repeats[index]):
                                    result = plans[index].run(graph, num_threads=threads)
                                    assert result.number_of_matches == counts[0]
                                    total += result.execution_time_seconds
                                walls[index].append((time.perf_counter() - start) / repeats[index])
                                samples[index].append(total / repeats[index])
                        measurements = []
                        for index, ((scheduler, bitmap), plan) in enumerate(zip(keys, plans)):
                            times = samples[index]
                            median = statistics.median(times)
                            measurements.append(dict(scheduler=scheduler, bitmap=bitmap,
                                selected="// bitmap-region build once" in plan.generated_code,
                                local_iterator="// bitmap local-index loop" in plan.generated_code,
                                full_region="// full bitmap region" in plan.generated_code,
                                fixed_words="count_local<bitmap_words>" in plan.generated_code,
                                code_sha256=hashlib.sha256(plan.generated_code.encode()).hexdigest(),
                                median_seconds=median,
                                mad_seconds=statistics.median(abs(t - median) for t in times),
                                samples_seconds=times, wall_samples_seconds=walls[index], repeats=repeats[index],
                                compilation=plan.compilation_profile))
                        row = dict(size=size, family=family, pattern=pattern, graph=graph_name,
                                   graph_metadata=graph_meta, threads=threads, count=counts[0],
                                   oracle_calibration_count=expected, measurements=measurements,
                                   process_peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                                   load_average=os.getloadavg())
                        output.write(json.dumps(row) + "\n")
                        output.flush()
                        print("SERVER_RESULT " + json.dumps(row), flush=True)


if __name__ == "__main__":
    main()
