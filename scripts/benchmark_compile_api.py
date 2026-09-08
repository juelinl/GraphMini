"""Measure actual compile_plan cache misses/hits with warm build dependencies.

Run serially per checkout with PYTHONPATH selecting the desired build. Only the
specific generated cache library returned by the API is temporarily moved; it
is restored afterwards. Process startup and graph construction are not timed.
"""
import argparse
import gc
import json
from pathlib import Path
import sys
import tempfile
import time

import numpy as np
import graphmini as gm

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from matching_oracle import count_matches, matrix
from runtime_test_support import restore_generated_plan_at_exit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    restore_generated_plan_at_exit()
    data = matrix(8, [(i, j) for i in range(8) for j in range(i + 1, 8)])
    query = matrix(7, [(i, i + 1) for i in range(6)] + [(0, 6)])
    expected = count_matches(data, query, False)
    graph = gm.Graph.from_csr(np.arange(0, 57, 7, dtype=np.uint64),
                             np.array([j for i in range(8) for j in range(8) if i != j], dtype=np.uint32))
    pattern = "".join(str(x) for row in query for x in row)

    def compile_once():
        start = time.perf_counter()
        plan = gm.compile_plan(graph, pattern, "edge", pruning_type="costmodel",
                               parallel_type="nested_rt", scheduler="graphpi")
        elapsed = time.perf_counter() - start
        assert plan.run(graph, num_threads=2).number_of_matches == expected
        path = Path(plan.module_path).resolve()
        del plan
        gc.collect()
        return elapsed, path

    initial, cache = compile_once()
    if not cache.is_file() or not any(p.name.startswith("python_plan_cache") for p in cache.parents):
        raise RuntimeError("Unexpected query cache path: " + str(cache))
    result = {"case": "cycle7_nested_costmodel", "repeats": args.repeats,
              "initial_call_seconds_cache_state_unspecified": initial,
              "expected_matches": expected, "cache_miss_seconds": [], "cache_hit_seconds": []}
    # Same filesystem as the cache, including servers with a separate /data.
    with tempfile.TemporaryDirectory(prefix="graphmini-api-bench-", dir=cache.parent) as scratch:
        backup = Path(scratch) / cache.name
        cache.replace(backup)
        try:
            for _ in range(args.repeats):
                # This is the benchmark's generated binary, not a source file.
                cache.unlink(missing_ok=True)
                elapsed, actual = compile_once()
                assert actual == cache
                result["cache_miss_seconds"].append(elapsed)
                elapsed, actual = compile_once()
                assert actual == cache
                result["cache_hit_seconds"].append(elapsed)
        finally:
            backup.replace(cache)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
