"""Compare untraced -O3 and -O3 -fno-inline queries without changing defaults."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import argparse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = root / ".verification/inlining"
    out.mkdir(parents=True, exist_ok=True)
    summary = {"commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
               "platform": platform.platform(), "stages": []}

    def run(name, command, env=None):
        print(name, flush=True)
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        summary["stages"].append(name)
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    # Reverse baseline/diagnostic order for the second backend.
    variants = [("pch", False), ("pch", True), ("backend", True), ("backend", False)]
    for mode, disabled in variants:
        name = ("noinline" if disabled else "inline") + "-" + mode
        build = root / ("build-" + name)
        command = ["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Release", "-DGRAPHMINI_BUILD_TESTS=ON",
                   "-DGRAPHMINI_PROFILE_QUERY_COMPILATION=OFF", "-DGRAPHMINI_EXPERIMENTAL_HEADER_UNITS=OFF",
                   "-DGRAPHMINI_EXPERIMENTAL_BACKEND_MODULE=" + ("ON" if mode == "backend" else "OFF"),
                   "-DGRAPHMINI_EXPERIMENTAL_TBB_MODULE=" + ("ON" if mode == "backend" else "OFF"),
                   "-DGRAPHMINI_EXPERIMENTAL_NO_INLINE=" + ("ON" if disabled else "OFF")]
        if args.compiler:
            command.append("-DCMAKE_CXX_COMPILER=" + args.compiler)
        run(name + "-configure", command)
        run(name + "-build", ["cmake", "--build", str(build), "--parallel", str(args.jobs)])
        run(name + "-ctest", ["ctest", "--test-dir", str(build), "--output-on-failure"])
        commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "plan_module"], text=True)
        query_command, = [c for c in commands.splitlines() if " -c " in c and " -o " in c
                          and "plan_module.dir" in c and "/plan.cpp" in c and "/plan.cppm" not in c]
        assert "-O3" in query_command and ("-fno-inline" in query_command) == disabled
        assert "-ftime-trace" not in query_command
        (out / (name + "-commands.txt")).write_text(commands)
        env = dict(os.environ, PYTHONPATH=str(build / "lib"))
        for kind in ["compile", "runtime"]:
            run(name + "-" + kind, [sys.executable, str(root / "scripts/benchmark_inlining.py"),
                                    "--kind", kind, "--build-dir", str(build),
                                    "--output", str(out / (name + "-" + kind + ".json"))], env)
        # Broader scheduler/pruning/semantic coverage for the changed variants.
        if disabled:
            run(name + "-oracle", [sys.executable, str(root / "tests/runtime_smoke.py"),
                                   "--results", str(out / (name + "-oracle.json"))], env)
    print("All inlining experiment stages passed:", out, flush=True)


if __name__ == "__main__":
    main()
