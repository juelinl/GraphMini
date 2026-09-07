"""Measure Ninja/Clang compilation stages without changing plans or build caches.

Build compilation_benchmark with GRAPHMINI_BUILD_TESTS=ON first. Run in the same
environment as the build. All compiler outputs live in a fresh temporary folder.
"""
import argparse
import json
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile
import time


def run(command, cwd):
    start = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    elapsed = time.perf_counter() - start
    if result.returncode:
        raise RuntimeError(f"{shlex.join(command)}\n{result.stdout}\n{result.stderr}")
    return elapsed, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-conda"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--sizes", type=int, nargs="+", default=[4], choices=range(4, 8))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    build = args.build_dir.resolve()
    _, commands = run(["ninja", "-t", "commands", "plan_module"], build)
    lines = commands.splitlines()
    pch = shlex.split(next(line for line in lines if "-emit-pch" in line))
    compile_cmd = shlex.split(next(line for line in lines if "-include-pch" in line))
    link_line = next(line for line in lines if "-dynamiclib" in line)
    # CMake's macOS Ninja rule wraps the actual link command with shell no-ops.
    link = shlex.split(link_line.removeprefix(": && ").removesuffix(" && :"))
    source = Path(compile_cmd[compile_cmd.index("-c") + 1])
    original_object = compile_cmd[compile_cmd.index("-o") + 1]

    def rewrite(command, output, input_path=None, use_pch=True):
        result = []
        i = 0
        while i < len(command):
            value = command[i]
            if value in ("-MT", "-MF"):
                i += 2
                continue
            if value == "-MD":
                i += 1
                continue
            if value == "-Xclang" and command[i + 1] in ("-include-pch", "-include"):
                kind, path = command[i + 1], command[i + 3]
                if use_pch:
                    result.extend(["-Xclang", kind, "-Xclang",
                                   str(pch_path) if kind == "-include-pch" else path])
                i += 4
                continue
            if value in ("-o", "-c"):
                replacement = output if value == "-o" else input_path
                result.extend([value, str(replacement or command[i + 1])])
                i += 2
                continue
            result.append(value)
            i += 1
        return result

    with tempfile.TemporaryDirectory(prefix="graphmini-compile-bench-") as scratch:
        scratch = Path(scratch)
        pch_path = scratch / "backend.pch"
        _, generated = run([str(build / "bin/compilation_benchmark"), str(scratch),
                            *map(str, args.sizes)], build)
        codegen = {parts[1]: float(parts[2]) for line in generated.splitlines()
                   if (parts := line.split()) and parts[0] == "BENCHMARK"}
        results = {"build_dir": str(build), "repeats": args.repeats, "sizes": args.sizes,
                   "compiler": run([compile_cmd[0], "--version"], build)[1].strip(),
                   "pch_build_seconds": [], "cases": {}}
        # Each invocation regenerates the PCH. This is artifact-cold, not an OS
        # filesystem-cache flush. Warm compile measurements reuse the last PCH.
        for _ in range(args.repeats):
            results["pch_build_seconds"].append(run(rewrite(pch, pch_path), build)[0])
        for name, seconds in codegen.items():
            item = {"codegen_mean_seconds": seconds,
                    "generated_source_bytes": (scratch / (name + ".cpp")).stat().st_size,
                    "pch_compile_seconds": [], "no_pch_compile_seconds": [],
                    "link_seconds": []}
            obj = scratch / (name + ".o")
            shared = scratch / (name + ".dylib")
            for repeat in range(args.repeats):
                # Alternate order to reduce systematic cache/thermal bias.
                for use_pch in ([True, False] if repeat % 2 == 0 else [False, True]):
                    command = rewrite(compile_cmd, obj, scratch / (name + ".cpp"), use_pch)
                    command.extend(["-I", str(source.parent)])
                    key = "pch_compile_seconds" if use_pch else "no_pch_compile_seconds"
                    item[key].append(run(command, build)[0])
                    if use_pch:
                        command = rewrite(link, shared)
                        command = [str(obj) if x == original_object else x for x in command]
                        item["link_seconds"].append(run(command, build)[0])
            results["cases"][name] = item
            print(name, {key: round(statistics.median(value), 4)
                         for key, value in item.items() if isinstance(value, list)}, flush=True)
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        print("PCH build median:", statistics.median(results["pch_build_seconds"]), "seconds")


if __name__ == "__main__":
    main()
