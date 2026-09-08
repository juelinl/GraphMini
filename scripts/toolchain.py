"""Shared source-install compiler selection; independent of Conda's CC/CXX defaults."""
import os
from pathlib import Path
import platform
import shutil


def default_compilers(system=None):
    system = system or platform.system()
    if system == "Darwin":
        return "clang", "clang++", "macos-clang"
    if system == "Linux":
        return "gcc", "g++", "linux-gcc"
    if system == "Windows":
        return "cl", "cl", "windows-msvc"
    raise RuntimeError(f"Unsupported installation platform: {system}")


def resolve_program(name):
    found = shutil.which(name)
    if not found:
        raise RuntimeError(f"Cannot find {name!r} on PATH. Activate the compiler environment; "
                           "on Windows use an x64 MSVC Developer PowerShell with Ninja installed.")
    # Keep the invoked name: resolving compiler symlinks can change driver mode.
    return os.path.abspath(found)


def select_toolchain(cc=None, cxx=None, generator=None):
    default_cc, default_cxx, tag = default_compilers()
    if bool(cc) != bool(cxx):
        raise RuntimeError("Specify --cc and --cxx together to avoid mixing toolchains.")
    if cc:
        tag = "custom"
    cc = resolve_program(cc or default_cc)
    cxx = resolve_program(cxx or default_cxx)
    generator = generator or ("Ninja" if shutil.which("ninja") else "Unix Makefiles")
    if platform.system() == "Windows" and generator != "Ninja":
        raise RuntimeError("Windows installation currently requires single-config Ninja with MSVC. "
                           "Visual Studio and Ninja Multi-Config builds are not supported by the runtime loader.")
    if generator not in ("Ninja", "Unix Makefiles"):
        raise RuntimeError("Use a single-config generator: Ninja or Unix Makefiles.")
    if generator == "Ninja":
        resolve_program("ninja")
    return cc, cxx, generator, tag


def check_build_cache(build_dir, cc, cxx, generator):
    cache_path = Path(build_dir) / "CMakeCache.txt"
    if not cache_path.exists():
        return
    cache = {}
    for line in cache_path.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    for key, requested in (("CMAKE_C_COMPILER", cc), ("CMAKE_CXX_COMPILER", cxx)):
        previous = cache.get(key)
        if previous and Path(previous).resolve() != Path(requested).resolve():
            raise RuntimeError(f"{build_dir} uses {key}={previous}, not {requested}. "
                               "Choose a fresh --build-dir; compiler caches cannot be switched safely.")
    if cache.get("CMAKE_GENERATOR", generator) != generator:
        raise RuntimeError(f"{build_dir} uses another generator. Choose a fresh --build-dir.")


def cmake_toolchain_args(cc, cxx, generator):
    return ["-G", generator, f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
            "-DCMAKE_BUILD_TYPE=Release"]
