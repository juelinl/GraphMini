"""Build, check, and benchmark PCH and named-module variants in the active environment.

Run serially per source tree: runtime compilation uses a shared generated file.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pch-build", type=Path, default=Path("build-conda"))
    parser.add_argument("--module-build", type=Path, default=Path("build-tbb-module"))
    parser.add_argument("--output-dir", type=Path, default=Path(".verification"))
    parser.add_argument("--compiler", help="Explicit C++ compiler, e.g. clang++ on Ubuntu")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    summary = {"platform": platform.platform(), "python": sys.version,
               "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
               "checks": []}

    def run(name, command, env=None):
        print(name, flush=True)
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        summary["checks"].append(name)
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    for mode, path in [("pch", args.pch_build.resolve()), ("named", args.module_build.resolve())]:
        configure = ["cmake", "-S", str(root), "-B", str(path), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", "-DGRAPHMINI_BUILD_TESTS=ON",
                     "-DGRAPHMINI_EXPERIMENTAL_HEADER_UNITS=OFF",
                     "-DGRAPHMINI_EXPERIMENTAL_TBB_MODULE=" + ("ON" if mode == "named" else "OFF")]
        if args.compiler:
            configure.append("-DCMAKE_CXX_COMPILER=" + args.compiler)
        run(mode + "-configure", configure)
        run(mode + "-build", ["cmake", "--build", str(path), "--parallel", str(args.jobs)])
        run(mode + "-ctest", ["ctest", "--test-dir", str(path), "--output-on-failure"])
        env = dict(os.environ, PYTHONPATH=str(path / "lib"))
        for size, script in [("small", "runtime_smoke.py"), ("large", "runtime_large.py")]:
            run(mode + "-" + size, [sys.executable, str(root / "tests" / script),
                                     "--results", str(out / (mode + "-" + size + ".json"))], env)
    run("benchmark", [sys.executable, str(root / "scripts/benchmark_compilation.py"),
                      "--build-dir", str(args.pch_build.resolve()), "--sizes", "6", "7",
                      "--repeats", str(args.repeats), "--tbb-module-source",
                      str(args.module_build.resolve() / "generated/tbb-module/tbb.cppm"),
                      "--driver-case", "cycle7_nested_costmodel", "--output", str(out / "benchmark.json")])
    print("All verification stages passed; results:", out, flush=True)


if __name__ == "__main__":
    main()
