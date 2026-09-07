"""Summarize completed no-inline measurements while preserving raw samples."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(root):
    output = {"run": json.loads((root / "summary.json").read_text()), "variants": {}}
    assert len(output["run"]["stages"]) == 22
    for mode in ["pch", "backend"]:
        for prefix in ["inline", "noinline"]:
            name = prefix + "-" + mode
            compilation = json.loads((root / (name + "-compile.json")).read_text())
            runtime = json.loads((root / (name + "-runtime.json")).read_text())
            assert len(compilation["cases"]) == len(runtime["cases"]) == 6
            variant = {"cases": {}}
            if prefix == "noinline":
                oracle = json.loads((root / (name + "-oracle.json")).read_text())
                assert not oracle["oracle_mismatches"] and len(oracle["results"]) == 432
                variant["oracle_executions"] = 432
                variant["oracle_mismatches"] = 0
            for case, values in compilation["cases"].items():
                assert len(values["misses"]) == len(values["hits"]) == 3
                assert all(not x["cache_hit"] for x in values["misses"])
                assert all(x["cache_hit"] for x in values["hits"])
                samples = {"api_miss": [x["api_seconds"] for x in values["misses"]],
                           "api_hit": [x["api_seconds"] for x in values["hits"]],
                           "build": [x["seconds"]["build"] for x in values["misses"]],
                           "scheduling": [x["seconds"]["scheduling"] for x in values["misses"]],
                           "compiler": [sum(e["seconds"] for e in x["edges"] if e["stage"] == "consumer_compile")
                                        for x in values["misses"]]}
                r = runtime["cases"][case]
                for threads, batches in r["threads"].items():
                    assert len(batches) == 5 and all(b["runs"] >= 3 for b in batches)
                    samples["execution_" + threads + "_threads"] = [b["mean_execution_seconds"] for b in batches]
                variant["cases"][case] = {"compile_graph_expected_matches": values["expected_matches"],
                                           "runtime_graph_expected_matches": r["expected_matches"],
                                           "samples_seconds": samples, "execution_batches": r["threads"],
                                           "median_seconds": {k: statistics.median(v) for k, v in samples.items()}}
            output["variants"][name] = variant
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.input_dir), indent=2))
