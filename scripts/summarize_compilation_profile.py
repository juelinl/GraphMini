"""Validate stage accounting and summarize a completed profiling run in seconds."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(root):
    result = {"run": json.loads((root / "summary.json").read_text()), "modes": {}}
    assert len(result["run"]["stages"]) == 14, "Profiling driver did not complete"
    for mode in ["pch", "backend"]:
        profile = json.loads((root / mode / "profile.json").read_text())
        control = json.loads((root / (mode + "-control-api.json")).read_text())
        output = {"profile_commit": profile["commit"], "control_api": control, "cases": {}}
        assert len(profile["cases"]) == 6
        for name, case in profile["cases"].items():
            assert len(case["misses"]) == len(case["hits"]) == profile["repeats"]
            for sample in case["misses"] + case["hits"]:
                times = sample["seconds"]
                assert times["codegen_total"] >= times["planning_total"] >= times["scheduling"] >= 0
                assert times["compile_total"] <= sample["api_wall_seconds"] + 1e-6
                exclusive = ["metadata_and_config", "codegen_total", "cache_lookup", "source_write",
                             "build", "library_copy", "library_load"]
                assert sum(times.get(k, 0) for k in exclusive) <= times["compile_total"] + 1e-6
                assert sample["cache_hit"] == ("build" not in times)
            samples = {"miss_api_wall": [s["api_wall_seconds"] for s in case["misses"]],
                       "hit_api_wall": [s["api_wall_seconds"] for s in case["hits"]]}
            for key in case["misses"][0]["seconds"]:
                samples[key] = [s["seconds"].get(key, 0) for s in case["misses"]]
            for key in ["build_system_check", "dependency_scan", "dependency_collation", "consumer_compile", "link"]:
                samples["ninja_" + key] = [sum(e["seconds"] for e in s["ninja_edges"] if e["stage"] == key)
                                          for s in case["misses"]]
            samples["build_residual"] = [s["seconds"]["build"] - sum(e["seconds"] for e in s["ninja_edges"])
                                         for s in case["misses"]]
            assert min(samples["build_residual"]) >= -0.005
            for key in ["ExecuteCompiler", "Frontend", "Backend", "Optimizer", "CodeGenPasses", "InlinerPass", "InstCombinePass"]:
                samples["clang_" + key] = [s["clang_totals_seconds"].get(key, 0) for s in case["misses"]]
            output["cases"][name] = {"expected_matches": case["expected_matches"], "samples_seconds": samples,
                                      "median_seconds": {k: statistics.median(v) for k, v in samples.items()}}
        result["modes"][mode] = output
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.input_dir), indent=2))
