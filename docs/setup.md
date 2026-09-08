# Installation and experimental builds

[Back to GraphMini](../README.md)

Run all commands below from the repository root. The Conda environment below
is the recommended installation method.

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
cmake -S . -B build-conda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python" -DOpenMP_ROOT="$CONDA_PREFIX" \
  -DGRAPHMINI_BUILD_TESTS=ON
python scripts/install_python.py --build-dir build-conda
cmake --build build-conda --parallel 6
ctest --test-dir build-conda --output-on-failure
```

Micromamba users can create the same environment with
`micromamba create -f environment.yml` and activate it with
`micromamba activate graphmini`. Use this setup instead of the virtual-environment
steps below. `build-conda` keeps its Python-specific build separate from `build`.

GraphMini requires **oneTBB 2023.1 or newer**. The installer above builds the
2023.1.0 tag into `.deps/oneTBB-2023.1.0`, including its preview `oneapi/tbb.cppm`
module interface. This is needed when Conda's platform packages are older; it
does not overwrite them. CMake prefers this local install. If your package
manager already provides a suitable version, the installer can be skipped.
The default still uses headers/PCH: installing the release does not itself
enable the official named module. Finished query-library caches are separated
by oneTBB version so an upgrade does not reuse old query binaries.

On the tested Clang 21/libc++ setup, upstream 2023.1.0's unmodified `tbb.cppm`
currently fails to compile: it unconditionally exports `cache_aligned_resource`
and `scalable_memory_resource`, while oneTBB's feature check disables those
declarations for libc++. An opt-in build-local workaround is available:

```bash
cmake -S . -B build-tbb-module -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHMINI_BUILD_TESTS=ON -DGRAPHMINI_EXPERIMENTAL_TBB_MODULE=ON
cmake --build build-tbb-module --target tbb_module_smoke
ctest --test-dir build-tbb-module -R '^tbb_module_smoke$' --output-on-failure
```

This requires CMake 3.28+ and upstream Clang/Ninja. CMake generates a copy at
`build-tbb-module/generated/tbb-module/tbb.cppm`, adding the existing feature
guard around just those two exports for 2023.1.0. Installed and vendor sources
remain untouched. The `graphmini_tbb_module` target exposes the named `tbb`
module; its smoke test imports it and exercises parallel execution and both
allocator types. With this option enabled, dynamic query plans also use
`import tbb` through the backend header. Build `pygraphmini` and `plan_module`
in this directory and set `PYTHONPATH=build-tbb-module/lib` to test that path.
The default, static plans, and profiling plans retain PCH. The named-module
and header-unit options are mutually exclusive.

To precompile GraphMini's backend as well, use the experimental backend module:

```bash
cmake -S . -B build-backend-module -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHMINI_BUILD_TESTS=ON -DGRAPHMINI_EXPERIMENTAL_BACKEND_MODULE=ON
cmake --build build-backend-module --parallel 6
ctest --test-dir build-backend-module --output-on-failure
PYTHONPATH=build-backend-module/lib python tests/runtime_large.py
```

This enables the oneTBB module automatically. Dynamic queries import
`graphmini.backend`, which exports the stable runtime types and re-exports `tbb`.
The interface preserves the header-defined types' identity for interoperability
with the C++17 Python host. Templates/inline methods remain available to the
optimizer; this is not a conversion of static/profiling plans or `std` to modules.
PCH remains the default. Use a separate build directory for each experimental
backend; finished query libraries also have backend-specific caches.

For a serial PCH/backend-module correctness and compilation/API comparison:

```bash
python scripts/verify_platform.py --backend-module \
  --module-build build-backend-module --output-dir .verification/backend
```

On Ubuntu, install upstream Clang and matching `clang-scan-deps` (for example,
Conda's `clangxx=21` and `clang-tools=21`) and add `--compiler clang++`.
See the [backend-module experiment results](../tests/benchmarks/backend-module-verification.md)
for correctness coverage, compilation timings, API timings, and limitations.
For the stage-level GraphPi/Ninja/Clang breakdown and profiling commands, see
[compilation profiling](../tests/benchmarks/compilation-profile.md).
The optional `GRAPHMINI_EXPERIMENTAL_NO_INLINE=ON` diagnostic keeps `-O3` but
adds `-fno-inline` to dynamic queries. It is off by default; see the
[compilation/execution tradeoff](../tests/benchmarks/no-inline-experiment.md)
before using it. It is not a selective inner-loop inlining policy.

Query-library caches include a fingerprint of runtime headers and compiler/build
settings, so runtime fixes do not silently reuse stale generated libraries.

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

### 2. Install `pygraphmini` into the environment

From the repository root:

```bash
python scripts/install_python.py
```

This is a source-tree install. The script:

1. configures CMake for the active Python interpreter
2. builds `pygraphmini` and the generated plan-module target
3. writes a `.pth` file into the environment so `import pygraphmini` resolves to this repository's build output

To remove that installation later:

```bash
python scripts/uninstall_python.py
```

You can override the interpreter or build directory if needed:

```bash
python scripts/install_python.py --python venv/bin/python --build-dir build
```

On Unix-like systems, the shell wrappers still work:

```bash
./scripts/install_python.sh
./scripts/uninstall_python.sh
```
