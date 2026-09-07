"""Build and profile PCH/backend modules in separate directories, serially.

Run in the dependency environment. Untraced API controls use the same flags
apart from Clang tracing. They are measurements, not overhead correction factors.
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
    parser.add_argument("--compiler")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output-dir", type=Path, default=Path(".verification/profiles"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    summary = {"commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
               "platform": platform.platform(), "python": sys.version, "stages": []}

    def run(name, command, env=None):
        print(name, flush=True)
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        summary["stages"].append(name)
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    for mode in ["pch", "backend"]:
        build = root / ("build-profile-" + mode)
        env = dict(os.environ, PYTHONPATH=str(build / "lib"))
        for traced in [False, True]:
            label = mode + ("-trace" if traced else "-control")
            command = ["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                       "-DCMAKE_BUILD_TYPE=Release", "-DGRAPHMINI_BUILD_TESTS=ON",
                       "-DGRAPHMINI_EXPERIMENTAL_HEADER_UNITS=OFF",
                       "-DGRAPHMINI_EXPERIMENTAL_TBB_MODULE=" + ("ON" if mode == "backend" else "OFF"),
                       "-DGRAPHMINI_EXPERIMENTAL_BACKEND_MODULE=" + ("ON" if mode == "backend" else "OFF"),
                       "-DGRAPHMINI_PROFILE_QUERY_COMPILATION=" + ("ON" if traced else "OFF")]
            if args.compiler:
                command.append("-DCMAKE_CXX_COMPILER=" + args.compiler)
            run(label + "-configure", command)
            run(label + "-build", ["cmake", "--build", str(build), "--parallel", str(args.jobs)])
            if traced:
                run(label + "-ctest", ["ctest", "--test-dir", str(build), "--output-on-failure"])
                run(label + "-profile", [sys.executable, str(root / "scripts/profile_compilation.py"),
                                        "--build-dir", str(build), "--output-dir", str(out / mode),
                                        "--repeats", str(args.repeats)], env)
            else:
                run(label + "-api", [sys.executable, str(root / "scripts/benchmark_compile_api.py"),
                                    "--output", str(out / (mode + "-control-api.json")),
                                    "--repeats", str(args.repeats)], env)
    print("All profiling stages passed:", out, flush=True)


if __name__ == "__main__":
    main()
