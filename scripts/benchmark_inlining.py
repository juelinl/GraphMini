"""Untraced compile/execute measurements for the inlining experiment.

Run compile and runtime modes in separate processes: pool state is not shared
between the tiny oracle graph and the larger synthetic execution graph.
"""
import argparse
import gc
import json
import math
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

import numpy as np
import pygraphmini as gm
from profile_compilation import ninja_edges, count_matches, matrix, restore_generated_plan_at_exit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=["compile", "runtime"], required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    restore_generated_plan_at_exit()
    build = args.build_dir.resolve()
    n = 8 if args.kind == "compile" else 14
    graph = gm.Graph.from_csr(np.arange(0, n * (n - 1) + 1, n - 1, dtype=np.uint64),
                             np.array([j for i in range(n) for j in range(n) if i != j], dtype=np.uint32))
    data = matrix(n, [(i, j) for i in range(n) for j in range(i + 1, n)])
    result = {"kind": args.kind, "graph_vertices": n, "cases": {}}
    for size in [6, 7]:
        for family in ["clique", "star", "cycle"]:
            name = f"{family}{size}_nested_costmodel"
            query = matrix(size, [(i, j) for i in range(size) for j in range(i + 1, size)
                                  if family == "clique" or (family == "star" and i == 0)
                                  or (family == "cycle" and (j == i + 1 or (i == 0 and j == size - 1)))])
            expected = (math.comb(n, size) if family == "clique" else
                        n * math.comb(n - 1, size - 1) if family == "star" else math.perm(n, size) // (2 * size))
            if args.kind == "compile":
                assert count_matches(data, query, False) == expected
            pattern = "".join(str(x) for row in query for x in row)

            def compile_once():
                start = time.perf_counter()
                plan = gm.compile_plan(graph, pattern, "edge_iep" if family == "star" else "edge",
                                       pruning_type="costmodel", parallel_type="nested_rt", scheduler="graphpi")
                sample = {"api_seconds": time.perf_counter() - start, **plan.compilation_profile}
                assert plan.run(graph, num_threads=1).number_of_matches == expected
                return plan, sample

            plan, _ = compile_once()  # dependencies warm; exclude bootstrap
            item = {"expected_matches": expected}
            if args.kind == "compile":
                cache = Path(plan.module_path).resolve()
                assert build in cache.parents and any(p.name.startswith("python_plan_cache") for p in cache.parents)
                del plan
                gc.collect()
                item.update(misses=[], hits=[])
                with tempfile.TemporaryDirectory(prefix="inlining-backup-", dir=cache.parent) as scratch:
                    backup = Path(scratch) / cache.name
                    cache.replace(backup)
                    try:
                        for _ in range(3):
                            cache.unlink(missing_ok=True)
                            subprocess.run(["ninja", "-C", str(build), "-t", "recompact"], capture_output=True, check=True)
                            before = (build / ".ninja_log").read_text().splitlines()
                            plan, sample = compile_once()
                            assert not sample["cache_hit"]
                            sample["edges"] = ninja_edges(before, (build / ".ninja_log").read_text().splitlines())
                            assert sum(e["stage"] == "consumer_compile" for e in sample["edges"]) == 1
                            assert not any(e["stage"] == "other" for e in sample["edges"])
                            item["misses"].append(sample)
                            del plan
                            gc.collect()
                            plan, sample = compile_once()
                            assert sample["cache_hit"] and "build" not in sample["seconds"]
                            item["hits"].append(sample)
                            del plan
                            gc.collect()
                    finally:
                        backup.replace(cache)
            else:
                item["threads"] = {}
                for threads in [1, 2]:
                    plan.run(graph, num_threads=threads)  # warm-up outside timer
                    batches = []
                    for _ in range(5):
                        durations = []
                        start = time.perf_counter()
                        while len(durations) < 128 and (len(durations) < 3 or time.perf_counter() - start < 0.1):
                            measured = plan.run(graph, num_threads=threads)
                            assert measured.number_of_matches == expected
                            durations.append(measured.execution_time_seconds)
                        batches.append({"runs": len(durations), "mean_execution_seconds": statistics.mean(durations)})
                    item["threads"][str(threads)] = batches
                del plan
                gc.collect()
            result["cases"][name] = item
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            print(args.kind, name, "passed", flush=True)


if __name__ == "__main__":
    main()
