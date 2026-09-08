#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
from toolchain import select_toolchain, check_build_cache, cmake_toolchain_args


def resolve_python_executable(python_bin: str) -> pathlib.Path:
    if python_bin in {"python", "python3"}:
        virtual_env = os.environ.get("VIRTUAL_ENV")
        if virtual_env:
            venv_python = pathlib.Path(virtual_env) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
            if venv_python.exists():
                return pathlib.Path(os.path.abspath(venv_python))

    candidate = pathlib.Path(python_bin)
    if candidate.is_absolute() or candidate.parent != pathlib.Path():
        return pathlib.Path(os.path.abspath(candidate))

    resolved = shutil.which(python_bin)
    if resolved is None:
        raise FileNotFoundError(f"Python interpreter not found: {python_bin}")
    return pathlib.Path(os.path.abspath(resolved))


def find_runtime_dirs(build_dir: pathlib.Path) -> list[pathlib.Path]:
    runtime_dirs = {build_dir / "lib", build_dir / "bin"}
    if os.name == "nt" and os.environ.get("CONDA_PREFIX"):
        runtime_dirs.add(pathlib.Path(os.environ["CONDA_PREFIX"]) / "Library" / "bin")
    patterns = [
        "libtbb*.so*",
        "libtbb*.dylib",
        "tbb*.dll",
        "libtbbmalloc*.so*",
        "libtbbmalloc*.dylib",
        "tbbmalloc*.dll",
    ]
    for pattern in patterns:
        for candidate in build_dir.rglob(pattern):
            runtime_dirs.add(candidate.parent.resolve())
    return sorted(path for path in runtime_dirs if path.exists())


def write_bootstrap(site_packages: pathlib.Path, module_dirs: list[pathlib.Path]) -> pathlib.Path:
    bootstrap_path = site_packages / "graphmini_dev_bootstrap.py"
    directories = ",\n    ".join(repr(str(path)) for path in module_dirs)
    bootstrap_path.write_text(
        "import os\n"
        "import pathlib\n"
        "import sys\n\n"
        "_GRAPHMINI_DIRS = [\n"
        f"    {directories}\n"
        "]\n"
        "for _path in _GRAPHMINI_DIRS:\n"
        "    if _path not in sys.path:\n"
        "        sys.path.append(_path)\n\n"
        "_DLL_HANDLES = []\n"
        "if hasattr(os, 'add_dll_directory'):\n"
        "    for _path in _GRAPHMINI_DIRS:\n"
        "        if pathlib.Path(_path).exists():\n"
        "            try:\n"
        "                _DLL_HANDLES.append(os.add_dll_directory(_path))\n"
        "            except OSError:\n"
        "                pass\n",
        encoding="utf-8",
    )
    return bootstrap_path


def write_pth(site_packages: pathlib.Path, module_dirs: list[pathlib.Path]) -> pathlib.Path:
    pth_path = site_packages / "graphmini_dev.pth"
    lines = [str(path) for path in module_dirs]
    lines.append("import graphmini_dev_bootstrap")
    pth_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return pth_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and install graphmini into a Python environment.")
    parser.add_argument("--python", dest="python_bin", default=sys.executable,
                        help="Python interpreter to install into. Default: current interpreter")
    parser.add_argument("--build-dir", default=None,
                        help="CMake build directory. Default: <repo>/build-<platform>-<compiler>")
    parser.add_argument("--generator", default=None,
                        help="Optional CMake generator. Default: use Ninja when available")
    parser.add_argument("--cc", help="Explicit C compiler (requires --cxx)")
    parser.add_argument("--cxx", help="Explicit C++ compiler (requires --cc)")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--tests", action="store_true", help="Build and run the CTest suite")
    parser.add_argument("--cmake-arg", action="append", default=[],
                        help="Extra CMake argument, e.g. --cmake-arg=-DTBB_DIR=/path/to/TBB")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    root_dir = pathlib.Path(__file__).resolve().parent.parent
    python_exe = resolve_python_executable(args.python_bin)
    cc, cxx, generator, tag = select_toolchain(args.cc, args.cxx, args.generator)
    build_dir = pathlib.Path(args.build_dir).resolve() if args.build_dir else root_dir / f"build-{tag}"
    check_build_cache(build_dir, cc, cxx, generator)
    build_dir.mkdir(parents=True, exist_ok=True)
    print(f"Installing with C={cc}, C++={cxx}, generator={generator}", flush=True)
    if os.name == "nt":
        print("Windows/MSVC installation is unverified; native build/runtime issues may remain.", flush=True)

    configure_cmd = [
        "cmake",
        "-S", str(root_dir),
        "-B", str(build_dir),
        *args.cmake_arg,
        *cmake_toolchain_args(cc, cxx, generator),
        f"-DPython3_EXECUTABLE={python_exe}",
    ]
    if args.tests:
        configure_cmd.append("-DGRAPHMINI_BUILD_TESTS=ON")
    build_cmd = ["cmake", "--build", str(build_dir), "--parallel", str(args.jobs)]
    if not args.tests:
        build_cmd += ["--target", "graphmini", "plan_module"]

    subprocess.run(configure_cmd, check=True)
    subprocess.run(build_cmd, check=True)
    if args.tests:
        subprocess.run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"], check=True)

    site_packages = pathlib.Path(
        subprocess.check_output(
            [str(python_exe), "-c", "import sysconfig; print(sysconfig.get_path('platlib'))"],
            text=True,
        ).strip()
    )
    site_packages.mkdir(parents=True, exist_ok=True)

    module_dirs = find_runtime_dirs(build_dir)
    bootstrap_path = write_bootstrap(site_packages, module_dirs)
    pth_path = write_pth(site_packages, module_dirs)

    print(f"Installed graphmini into {python_exe}")
    print(f"Wrote {pth_path}")
    print(f"Wrote {bootstrap_path}")
    subprocess.run([str(python_exe), "-c",
                    "import sys; from pathlib import Path; import graphmini; "
                    "assert Path(graphmini.__file__).resolve().parent == Path(sys.argv[1]).resolve(), "
                    "'Another graphmini install or PYTHONPATH entry shadows this build: ' + graphmini.__file__; "
                    "print(graphmini.__file__)", str(build_dir / "lib")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
