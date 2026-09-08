"""Compare server JSONL runs, requiring matching counts and graph fingerprints."""
import argparse
import json
import math
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+")
    parser.add_argument("--output", required=True)
    parser.add_argument("--require-runtime-pairs", action="store_true",
                        help="Reject cases missing either revision's array/bitmap controls")
    args = parser.parse_args()
    cases, metadata = {}, []
    for filename in args.runs:
        lines = [json.loads(line) for line in Path(filename).read_text().splitlines()]
        meta = lines[0]["metadata"]
        metadata.append(meta)
        for row in lines[1:]:
            key = (row["size"], row["family"], row["graph"], row["threads"])
            case = cases.setdefault(key, dict(count=row["count"], graph_metadata=row["graph_metadata"],
                                               pattern=row["pattern"], measurements={}))
            assert (case["count"], case["graph_metadata"], case["pattern"]) == (
                row["count"], row["graph_metadata"], row["pattern"]), (filename, key, "mismatch")
            for m in row["measurements"]:
                if m["scheduler"] == "outgoing" and not m["bitmap"]:
                    original = case.setdefault("array_code_sha256", m["code_sha256"])
                    assert original == m["code_sha256"], (filename, key, "array control source changed")
                name = f'{meta["label"]}:{m["scheduler"]}:{"bitmap" if m["bitmap"] else "array"}'
                case["measurements"].setdefault(name, []).append(m)
    result = []
    for (size, family, graph, threads), case in sorted(cases.items()):
        merged = {}
        for name, runs in case.pop("measurements").items():
            assert len({r["code_sha256"] for r in runs}) == 1, (name, "plan changed between runs")
            merged[name] = dict(seconds=statistics.median(r["median_seconds"] for r in runs),
                                round_medians=[r["median_seconds"] for r in runs],
                                selected=runs[0]["selected"], runs=len(runs),
                                median_relative_mad=statistics.median(
                                    r["mad_seconds"] / r["median_seconds"] for r in runs))
        row = dict(size=size, family=family, graph=graph, threads=threads, **case, measurements=merged)
        current = merged.get("current:outgoing:bitmap")
        previous = merged.get("previous:outgoing:bitmap")
        array = merged.get("current:outgoing:array")
        old_array = merged.get("previous:outgoing:array")
        if args.require_runtime_pairs:
            assert current and previous and array and old_array, (size, family, graph, "missing runtime pair")
        if current and previous:
            assert current["selected"] == previous["selected"], (size, family, "selection changed")
            row["refinement_speedup"] = previous["seconds"] / current["seconds"]
        if current and array:
            row["bitmap_vs_array"] = array["seconds"] / current["seconds"]
        if current and previous and array and old_array:
            row["array_control_ratio"] = old_array["seconds"] / array["seconds"]
            row["refinement_speedup_normalized"] = row["refinement_speedup"] / row["array_control_ratio"]
        result.append(row)
    summary = {}
    for graph in sorted({r["graph"] for r in result}):
        rows = [r for r in result if r["graph"] == graph and "refinement_speedup" in r
                and r["measurements"]["current:outgoing:bitmap"]["selected"]]
        if not rows:
            continue
        summary[graph] = dict(cases=len(rows), positive_counts=sum(r["count"] > 0 for r in rows))
        for metric in ("refinement_speedup", "refinement_speedup_normalized", "bitmap_vs_array"):
            values = [r[metric] for r in rows if metric in r]
            if values:
                summary[graph][metric] = dict(geomean=math.exp(statistics.mean(math.log(v) for v in values)),
                                             minimum=min(values), maximum=max(values),
                                             faster_5pct=sum(v > 1.05 for v in values),
                                             slower_5pct=sum(v < 1 / 1.05 for v in values))
    output = dict(metadata=metadata, summary=summary, cases=result)
    Path(args.output).write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    paired = sum("refinement_speedup" in row for row in result)
    print(f"Validated {len(result)} cases; {paired} have paired runtime revisions")


if __name__ == "__main__":
    main()
