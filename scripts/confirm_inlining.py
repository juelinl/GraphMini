"""Repeat the seven-cycle comparison in forward/reverse order using existing builds."""
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / ".verification/inlining/confirmation"
    out.mkdir(parents=True, exist_ok=True)
    variants = ["inline-pch", "noinline-pch", "inline-backend", "noinline-backend"]
    result = {"case": "cycle7_nested_costmodel", "rounds": []}
    for order in [variants, list(reversed(variants))]:
        current = {"order": order, "variants": {}}
        for variant in order:
            build = root / ("build-" + variant)
            cache = (build / "CMakeCache.txt").read_text()
            assert "CMAKE_BUILD_TYPE:STRING=Release" in cache
            expected = "ON" if variant.startswith("noinline") else "OFF"
            assert "GRAPHMINI_EXPERIMENTAL_NO_INLINE:BOOL=" + expected in cache
            assert "GRAPHMINI_PROFILE_QUERY_COMPILATION:BOOL=OFF" in cache
            current["variants"][variant] = {}
            for kind in ["compile", "runtime"]:
                name = f"{len(result['rounds'])}-{variant}-{kind}"
                print(name, flush=True)
                target = out / (name + ".json")
                with (out / (name + ".log")).open("w") as log:
                    subprocess.run([sys.executable, str(root / "scripts/benchmark_inlining.py"),
                                    "--kind", kind, "--case", result["case"], "--build-dir", str(build),
                                    "--output", str(target)], cwd=root,
                                   env=dict(os.environ, PYTHONPATH=str(build / "lib")),
                                   stdout=log, stderr=subprocess.STDOUT, check=True)
                current["variants"][variant][kind] = json.loads(target.read_text())["cases"][result["case"]]
        result["rounds"].append(current)
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Alternating-order confirmation passed", flush=True)


if __name__ == "__main__":
    main()
