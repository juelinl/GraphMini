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
import sys
from build_header_unit import header_unit_command


def run(command, cwd):
    start = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    elapsed = time.perf_counter() - start
    if result.returncode:
        raise RuntimeError(f"{shlex.join(command)}\n{result.stdout}\n{result.stderr}")
    return elapsed, result.stdout


def measure_driver(scratch, build, compile_command, link_command, obj, shared, repeats):
    """Cache-miss build-driver + load timing, with precompiled dependencies reused."""
    project = scratch / "driver-project"
    driver = scratch / "driver-build"
    project.mkdir(exist_ok=True)
    def quote(value):
        return "[==[" + str(value) + "]==]"
    # A minimal CMake/Ninja target uses the exact measured compiler/linker commands.
    # This isolates driver overhead; it is not the Python compile_plan API itself.
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(PlanDriver NONE)\n"
        f"add_custom_command(OUTPUT {quote(shared)}\n"
        " COMMAND " + " ".join(map(quote, compile_command)) + "\n"
        " COMMAND " + " ".join(map(quote, link_command)) + "\n"
        f" WORKING_DIRECTORY {quote(build)} VERBATIM)\n"
        f"add_custom_target(plan_module DEPENDS {quote(shared)})\n")
    run(["cmake", "-S", str(project), "-B", str(driver), "-G", "Ninja"], build)
    samples = []
    for _ in range(repeats):
        # Only remove benchmark-owned artifacts in its fresh temporary directory.
        obj.unlink(missing_ok=True)
        shared.unlink(missing_ok=True)
        seconds, _ = run(["cmake", "--build", str(driver), "--target", "plan_module"], build)
        _, loaded = run([sys.executable, "-c",
                         "import ctypes,sys,time; t=time.perf_counter(); "
                         "m=ctypes.CDLL(sys.argv[1]); "
                         "print(time.perf_counter()-t)", str(shared)], build)
        samples.append({"build_seconds": seconds, "load_seconds": float(loaded),
                        "build_plus_load_seconds": seconds + float(loaded)})
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-conda"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--sizes", type=int, nargs="+", default=[4], choices=range(4, 8))
    parser.add_argument("--header-units", action="store_true",
                        help="Compare C++20 header units, PCH, and textual headers with identical flags")
    parser.add_argument("--driver-case", help="Also time CMake build and library load for this case name")
    parser.add_argument("--tbb-module-source", type=Path,
                        help="Compare the generated, compatibility-patched official tbb.cppm with C++20 PCH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend-module-source", type=Path,
                        help="Also compare the complete backend module; requires --tbb-module-source")
    args = parser.parse_args()
    if args.backend_module_source and not args.tbb_module_source:
        parser.error("--backend-module-source requires --tbb-module-source")
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    build = args.build_dir.resolve()
    _, commands = run(["ninja", "-t", "commands", "plan_module"], build)
    lines = commands.splitlines()
    pch = shlex.split(next(line for line in lines if "-emit-pch" in line))
    compile_cmd = shlex.split(next(line for line in lines if "-include-pch" in line))
    if args.header_units or args.tbb_module_source:
        pch = ["-std=gnu++20" if x.startswith("-std=") else x for x in pch]
        compile_cmd = ["-std=gnu++20" if x.startswith("-std=") else x for x in compile_cmd]
    link_line = next(line for line in lines if "-dynamiclib" in line or " -shared " in line)
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
        unit_path = scratch / "backend.pcm"
        named_pcm = scratch / "tbb.pcm"
        named_obj = scratch / "tbb.o"
        backend_pcm = scratch / "graphmini.backend.pcm"
        backend_obj = scratch / "graphmini.backend.o"
        backend_flags = ["-DGRAPHMINI_USE_BACKEND_MODULE=1",
                         f"-fmodule-file=tbb={named_pcm}",
                         f"-fmodule-file=graphmini.backend={backend_pcm}"]
        _, generated = run([str(build / "bin/compilation_benchmark"), str(scratch),
                            *map(str, args.sizes)], build)
        codegen = {parts[1]: float(parts[2]) for line in generated.splitlines()
                   if (parts := line.split()) and parts[0] == "BENCHMARK"}
        if args.driver_case and args.driver_case not in codegen:
            parser.error("--driver-case must name a generated benchmark case")
        results = {"build_dir": str(build), "repeats": args.repeats, "sizes": args.sizes,
                   "language_standard": "gnu++20" if args.header_units or args.tbb_module_source else "build default",
                   "compiler": run([compile_cmd[0], "--version"], build)[1].strip(),
                   "pch_build_seconds": [], "cases": {}}
        # Each invocation regenerates the PCH. This is artifact-cold, not an OS
        # filesystem-cache flush. Warm compile measurements reuse the last PCH.
        for _ in range(args.repeats):
            results["pch_build_seconds"].append(run(rewrite(pch, pch_path), build)[0])
        if args.header_units:
            unit_command = rewrite(compile_cmd, scratch / "unused.o", use_pch=False)
            unit_command.append("-DGRAPHMINI_USE_HEADER_UNIT=1")
            unit_command = header_unit_command(unit_command, source.parent.parent / "backend/backend.h",
                                               unit_path)
            results["header_unit_build_seconds"] = [
                run(unit_command, build)[0] for _ in range(args.repeats)]
        if args.tbb_module_source:
            named_source = args.tbb_module_source.resolve()
            command = rewrite(compile_cmd, named_pcm, named_source, use_pch=False)
            command[command.index("-c")] = "--precompile"
            object_command = rewrite(compile_cmd, named_obj, named_pcm, use_pch=False)
            results["named_module_bmi_seconds"], results["named_module_object_seconds"] = [], []
            for _ in range(args.repeats):
                results["named_module_bmi_seconds"].append(run(command, build)[0])
                results["named_module_object_seconds"].append(run(object_command, build)[0])
        if args.backend_module_source:
            command = rewrite(compile_cmd, backend_pcm, args.backend_module_source.resolve(), use_pch=False)
            command[command.index("-c")] = "--precompile"
            command.append(f"-fmodule-file=tbb={named_pcm}")
            object_command = rewrite(compile_cmd, backend_obj, backend_pcm, use_pch=False)
            object_command.append(f"-fmodule-file=tbb={named_pcm}")
            results["backend_module_bmi_seconds"], results["backend_module_object_seconds"] = [], []
            for _ in range(args.repeats):
                results["backend_module_bmi_seconds"].append(run(command, build)[0])
                results["backend_module_object_seconds"].append(run(object_command, build)[0])
        for name, seconds in codegen.items():
            item = {"codegen_mean_seconds": seconds,
                    "generated_source_bytes": (scratch / (name + ".cpp")).stat().st_size,
                    "pch_compile_seconds": [], "no_pch_compile_seconds": [],
                    "link_seconds": []}
            if args.header_units:
                item.update(header_unit_compile_seconds=[], header_unit_link_seconds=[])
            if args.tbb_module_source:
                item.update(named_module_compile_seconds=[], named_module_link_seconds=[])
            if args.backend_module_source:
                item.update(backend_module_compile_seconds=[], backend_module_link_seconds=[])
            obj = scratch / (name + ".o")
            shared = scratch / (name + Path(link[link.index("-o") + 1]).suffix)
            for repeat in range(args.repeats):
                # Alternate order to reduce systematic cache/thermal bias.
                modes = ["pch", "no_pch"] + (["header_unit"] if args.header_units else [])
                if args.tbb_module_source:
                    modes.append("named_module")
                if args.backend_module_source:
                    modes.append("backend_module")
                modes = modes[repeat % len(modes):] + modes[:repeat % len(modes)]
                for mode in modes:
                    use_pch = mode == "pch"
                    command = rewrite(compile_cmd, obj, scratch / (name + ".cpp"), use_pch)
                    command.extend(["-I", str(source.parent)])
                    if mode == "header_unit":
                        command.extend(["-DGRAPHMINI_USE_HEADER_UNIT=1", f"-fmodule-file={unit_path}"])
                    if mode == "named_module":
                        command.extend(["-DGRAPHMINI_USE_TBB_MODULE=1", f"-fmodule-file=tbb={named_pcm}"])
                    if mode == "backend_module":
                        command.extend(backend_flags)
                    key = mode + "_compile_seconds"
                    item[key].append(run(command, build)[0])
                    if mode != "no_pch":
                        command = rewrite(link, shared)
                        command = [str(obj) if x == original_object else x for x in command]
                        if mode == "named_module":
                            command.append(str(named_obj))
                        if mode == "backend_module":
                            command.extend([str(named_obj), str(backend_obj)])
                        key = "link_seconds" if use_pch else mode + "_link_seconds"
                        item[key].append(run(command, build)[0])
            results["cases"][name] = item
            if name == args.driver_case:
                driver_modes = ["pch"] + (["header_unit"] if args.header_units else [])
                if args.tbb_module_source:
                    driver_modes.append("named_module")
                if args.backend_module_source:
                    driver_modes.append("backend_module")
                for mode in driver_modes:
                    command = rewrite(compile_cmd, obj, scratch / (name + ".cpp"), mode == "pch")
                    command.extend(["-I", str(source.parent)])
                    if mode == "header_unit":
                        command.extend(["-DGRAPHMINI_USE_HEADER_UNIT=1", f"-fmodule-file={unit_path}"])
                    if mode == "named_module":
                        command.extend(["-DGRAPHMINI_USE_TBB_MODULE=1", f"-fmodule-file=tbb={named_pcm}"])
                    if mode == "backend_module":
                        command.extend(backend_flags)
                    link_command = [str(obj) if x == original_object else x for x in rewrite(link, shared)]
                    if mode == "named_module":
                        link_command.append(str(named_obj))
                    if mode == "backend_module":
                        link_command.extend([str(named_obj), str(backend_obj)])
                    item[mode + "_driver_samples"] = measure_driver(
                        scratch, build, command, link_command, obj, shared, args.repeats)
            print(name, {key: round(statistics.median(value), 4)
                         for key, value in item.items() if isinstance(value, list)
                         and value and isinstance(value[0], (int, float))}, flush=True)
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        print("PCH build median:", statistics.median(results["pch_build_seconds"]), "seconds")


if __name__ == "__main__":
    main()
