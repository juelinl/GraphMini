"""Build the oneTBB 2023.1.0 release locally when package channels lag upstream.

Run in the GraphMini Conda environment. Does not replace Conda/system packages.
"""
import argparse
from pathlib import Path
import subprocess
from toolchain import select_toolchain, check_build_cache, cmake_toolchain_args

VERSION = "2023.1.0"
COMMIT = "3046c8b0c29df995980003ea24f4d78c80ec0c8d"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--generator")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--prefix", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    root = Path(__file__).resolve().parents[1]
    deps = root / ".deps"
    deps.mkdir(exist_ok=True)
    source = deps / f"oneTBB-src-{VERSION}"
    cc, cxx, generator, tag = select_toolchain(args.cc, args.cxx, args.generator)
    build = args.build_dir.resolve() if args.build_dir else deps / f"oneTBB-build-{VERSION}-{tag}"
    prefix = args.prefix.resolve() if args.prefix else deps / f"oneTBB-{VERSION}"
    check_build_cache(build, cc, cxx, generator)
    if not source.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", f"v{VERSION}",
                        "https://github.com/uxlfoundation/oneTBB.git", str(source)], check=True)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=source, text=True).strip()
    if actual != COMMIT or dirty:
        raise RuntimeError(f"Refusing to reuse modified or unexpected oneTBB source: {source}")
    subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                    *cmake_toolchain_args(cc, cxx, generator), f"-DCMAKE_INSTALL_PREFIX={prefix}",
                    "-DTBB_TEST=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)
    if not (prefix / "include/oneapi/tbb.cppm").is_file():
        raise RuntimeError("The installed release is missing tbb.cppm")
    print(f"Installed oneTBB {VERSION} at {prefix}. Reconfigure/rebuild GraphMini next.")


if __name__ == "__main__":
    main()
