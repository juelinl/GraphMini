"""Profile GraphPi query compilation with warm dependencies, misses and hits.

Run serially per checkout. Uses the extension selected by PYTHONPATH; the build
must enable GRAPHMINI_PROFILE_QUERY_COMPILATION. Preserves generated source and
temporarily backed-up query cache libraries. Raw Clang traces are copied out.
"""
import argparse
import gc
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
import pygraphmini as gm

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from matching_oracle import count_matches, matrix
from runtime_test_support import restore_generated_plan_at_exit


def ninja_edges(before, after):
    if after[:len(before)] != before:
        raise RuntimeError("Ninja log compacted during sample; rerun after warm-up")
    edges = {}
    for line in after[len(before):]:
        if line.startswith("#"):
            continue
        start, end, _, output, command_hash = line.split("\t")
        key = (start, end, command_hash)
        edge = edges.setdefault(key, {"seconds": (int(end) - int(start)) / 1000, "outputs": []})
        edge["outputs"].append(output)
    result = list(edges.values())
    for edge in result:
        paths = edge["outputs"]
        if any(p.endswith("cmake.verify_globs") for p in paths):
            edge["stage"] = "build_system_check"
        elif any(p.endswith(".ddi") for p in paths):
            edge["stage"] = "dependency_scan"
        elif any(p.endswith(".dd") for p in paths):
            edge["stage"] = "dependency_collation"
        elif any(p.endswith("plan.cpp.o") for p in paths):
            edge["stage"] = "consumer_compile"
        elif any(Path(p).name in ("libplan_module.so", "libplan_module.dylib") for p in paths):
            edge["stage"] = "link"
        else:
            edge["stage"] = "other"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    restore_generated_plan_at_exit()
    build, out = args.build_dir.resolve(), args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    graph = gm.Graph.from_csr(np.arange(0, 57, 7, dtype=np.uint64),
                             np.array([j for i in range(8) for j in range(8) if i != j], dtype=np.uint32))
    data = matrix(8, [(i, j) for i in range(8) for j in range(i + 1, 8)])
    log = build / ".ninja_log"
    object_dir = build / "src/codegen_output/CMakeFiles/plan_module.dir"
    result = {"build": str(build), "repeats": args.repeats, "scheduler": "graphpi",
              "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
              "graph": "complete eight-vertex graph", "cases": {}}
    for size in [6, 7]:
        for family in ["clique", "star", "cycle"]:
            name = f"{family}{size}_nested_costmodel"
            query = matrix(size, [(i, j) for i in range(size) for j in range(i + 1, size)
                                  if family == "clique" or (family == "star" and i == 0)
                                  or (family == "cycle" and (j == i + 1 or (i == 0 and j == size - 1)))])
            pattern = "".join(str(x) for row in query for x in row)
            expected = count_matches(data, query, False)

            def compile_once():
                start = time.perf_counter()
                plan = gm.compile_plan(graph, pattern, "edge_iep" if family == "star" else "edge",
                                       pruning_type="costmodel", parallel_type="nested_rt", scheduler="graphpi")
                elapsed = time.perf_counter() - start
                profile = plan.compilation_profile
                cache = Path(plan.module_path).resolve()
                assert plan.run(graph, num_threads=2).number_of_matches == expected
                del plan
                gc.collect()
                return {"api_wall_seconds": elapsed, **profile}, cache

            seed, cache = compile_once()  # untimed bootstrap: dependencies and query library
            if build not in cache.parents or not any(p.name.startswith("python_plan_cache") for p in cache.parents):
                raise RuntimeError("Unexpected query cache path: " + str(cache))
            case = {"expected_matches": expected, "bootstrap": seed, "misses": [], "hits": []}
            with tempfile.TemporaryDirectory(prefix="profile-backup-", dir=cache.parent) as scratch:
                backup = Path(scratch) / cache.name
                cache.replace(backup)
                try:
                    for repetition in range(args.repeats):
                        cache.unlink(missing_ok=True)  # benchmark-generated library only
                        before = log.read_text().splitlines()
                        trace_start = time.time_ns()
                        miss, actual = compile_once()
                        assert actual == cache and not miss["cache_hit"]
                        miss["ninja_edges"] = ninja_edges(before, log.read_text().splitlines())
                        assert sum(e["stage"] == "consumer_compile" for e in miss["ninja_edges"]) == 1
                        assert sum(e["stage"] == "link" for e in miss["ninja_edges"]) == 1
                        if any(e["stage"] == "other" for e in miss["ninja_edges"]):
                            raise RuntimeError("Unexpected rebuild in warm-dependency sample")
                        traces = [p for p in object_dir.rglob("*.json") if p.stat().st_mtime_ns >= trace_start]
                        if len(traces) != 1:
                            raise RuntimeError(f"Expected one fresh consumer Clang trace, found {traces}")
                        trace = json.loads(traces[0].read_text())
                        miss["clang_totals_seconds"] = {e["name"].removeprefix("Total "): e["dur"] / 1e6
                                                        for e in trace["traceEvents"] if e["name"].startswith("Total ")}
                        trace_name = f"{name}-{repetition}.trace.json"
                        shutil.copyfile(traces[0], out / trace_name)
                        miss["trace_file"] = trace_name
                        case["misses"].append(miss)
                        hit, actual = compile_once()
                        assert actual == cache and hit["cache_hit"] and "build" not in hit["seconds"]
                        case["hits"].append(hit)
                finally:
                    backup.replace(cache)
            result["cases"][name] = case
            (out / "profile.json").write_text(json.dumps(result, indent=2) + "\n")
            print(name, "profiled", flush=True)


if __name__ == "__main__":
    main()
