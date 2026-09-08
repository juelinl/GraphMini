# Installation and experimental builds

[Back to GraphMini](../README.md)

Run all commands below from the repository root. The Conda environment below
is the recommended installation method.

## Compiler policy and verification status

| Platform | Default installation compiler | Verification |
|---|---|---|
| macOS | Clang / `clang++` | Verified with upstream Clang 21.1.8 on macOS arm64. AppleClang can be selected for PCH but was not reverified in this update. |
| Ubuntu/Linux | GCC / `g++` | Verified with Conda GCC/G++ 15.3.0 on Ubuntu 22.04 x86-64 (`ssh jupiter`), not Ubuntu's system GCC 12. |
| Windows | MSVC / `cl.exe` | **Unverified.** Compiler selection is tested with mocks only; no Windows build or runtime has been validated. |

Both `install_python.py` and `install_onetbb.py` explicitly select these compilers
from PATH, rather than inheriting `CC`/`CXX` from another environment. Activate
the intended compiler environment first. Matching `--cc` and `--cxx` overrides
are available for experiments; do not mix compilers within a build directory.
The scripts refuse a cached compiler or generator mismatch and ask for a fresh
`--build-dir`. Installations use Release and headers/PCH by default.

The default Python build directories are `build-macos-clang`, `build-linux-gcc`,
and `build-windows-msvc`. Explicit CMake commands do not apply the script's
compiler policy automatically: pass `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` yourself.

## Installation options

### Conda environment

The repository includes `environment.yml` with Python 3.14 and current compatible
releases of NumPy, CMake, Ninja, the C++ compiler, clang-format, OpenMP, oneTBB,
fmt, cxxopts, and pybind11. On macOS, Xcode Command Line Tools provide the SDK.
CMake discovers the installed packages; it does not fetch pinned dependency copies.

```bash
conda env create -f environment.yml
conda activate graphmini
python scripts/install_onetbb.py
python scripts/install_python.py --tests --jobs 6
python -c "import graphmini; print(graphmini.__file__)"
```

Micromamba users can create the same environment with
`micromamba create -f environment.yml` and activate it with
`micromamba activate graphmini`. Use this setup instead of the virtual-environment
steps below. Compiler-specific directories keep this installation separate from
older Clang/module experiments. Use the same compiler family and compatible C++
runtime for dependencies and GraphMini; on Ubuntu the active environment must
provide `gcc` and `g++`, not just Clang.

For end-to-end counting verification, run serially from the repository root:

```bash
# macOS; replace with build-linux-gcc on Ubuntu.
PYTHONPATH=build-macos-clang/lib python tests/runtime_smoke.py
PYTHONPATH=build-macos-clang/lib python tests/runtime_large.py
```

`--tests` builds all CMake test targets and runs CTest; it does not run these
longer Python runtime suites. They compare counts to an independent,
symmetry-normalized oracle, including six- and seven-vertex patterns.

GraphMini requires **oneTBB 2023.1 or newer**. The installer above builds the
2023.1.0 tag into `.deps/oneTBB-2023.1.0`, including its preview `oneapi/tbb.cppm`
module interface. This is needed when Conda's platform packages are older; it
does not overwrite them. CMake prefers this local install. If your package
manager already provides a suitable version, the installer can be skipped.
The default still uses headers/PCH: installing the release does not itself
enable the official named module. Finished query-library caches are separated
by oneTBB version so an upgrade does not reuse old query binaries.

The oneTBB installer uses a compiler-specific build directory as well. To retain
an existing dependency installation, pass `--prefix /another/prefix` and select
it for GraphMini with `--cmake-arg=-DTBB_DIR=/another/prefix/lib/cmake/TBB`.

## Windows/MSVC: unverified installation path

Install Visual Studio C++ Build Tools, the Windows SDK, Ninja, Python/NumPy,
and MSVC-compatible builds of the C++ dependencies. Use an **x64 Developer
PowerShell for Visual Studio**, where `cl.exe` and `ninja` are on PATH:

```powershell
python scripts/install_onetbb.py
python scripts/install_python.py --tests --jobs 6
```

The installer uses MSVC with single-config Ninja, not MinGW or Visual Studio's
multi-config generator. It supplies MSVC-style optimization flags and places the
query DLL beside the extension. These accommodations are **not evidence of
Windows compatibility**: native compilation, OpenMP behavior, DLL dependencies,
and runtime query compilation remain unverified and may require further fixes.
`environment.yml` has only been exercised on macOS and Ubuntu; Windows dependency
provisioning may also require adjustments.
Do not use the Unix shell wrappers on Windows.

## Manual installation

### 1. Create a Python environment

For a manual setup, first install oneTBB (including development headers and
tbbmalloc), fmt, cxxopts, pybind11's CMake package, OpenMP, and a C++17 compiler.
Set `CMAKE_PREFIX_PATH` to their installation prefix if needed. The Conda setup
above installs these dependencies together.

```bash
python3 -m venv venv
source venv/bin/activate
pip install numpy cmake ninja clang-format
```

### 2. Install `graphmini` into the environment

The Python extension and CMake target were renamed from `pygraphmini` to
`graphmini`. Rebuild/reinstall and update imports to `import graphmini as gm`.
No compatibility alias is provided for the old name. Existing build directories
may still contain an old `pygraphmini` binary; do not import both extensions in
the same interpreter.

From the repository root:

```bash
python scripts/install_python.py
```

This is a source-tree install. The script:

1. selects the platform compiler, checks the existing cache, and configures Release for the active Python interpreter
2. builds `graphmini` and the generated plan-module target
3. writes a `.pth` file into the environment so `import graphmini` resolves to this repository's build output

To remove that installation later:

```bash
python scripts/uninstall_python.py
```

You can override the interpreter or build directory if needed:

```bash
python scripts/install_python.py --python venv/bin/python --build-dir build
```

For an explicit compiler override, provide both drivers, e.g.
`--cc clang --cxx clang++ --build-dir build-clang-custom`.
Additional dependency options can be passed as `--cmake-arg=-DCMAKE_PREFIX_PATH=/prefix`.

On Unix-like systems, the shell wrappers still work:

```bash
./scripts/install_python.sh
./scripts/uninstall_python.sh
```
