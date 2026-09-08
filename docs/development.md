# Platforms, verification, and runtime internals

[Back to GraphMini](../README.md)

Run commands from the repository root in your build environment.

## Requirements

### Large benchmark datasets

The original large-dataset workflow calls for 128 GB of free RAM to preprocess
Friendster and 180 GB of free disk space for preprocessed graphs. These are not
requirements for small graphs or the quick-start example.

### Supported OS

Current status:

1. Ubuntu 22.04: supported
2. Ubuntu 24.04: supported
3. WSL on Windows: known to work
4. macOS arm64: builds and small-graph runtime checks verified with the Conda environment
5. native Windows: portability changes have been added, but this remains untested

Platform outlook:

- Linux support is already in place.
- macOS arm64 has been verified with compiler regression and runtime compilation tests.
- native Windows is more plausible now because shared-module loading, process helpers, and graph memory mapping no longer assume POSIX-only APIs.

Remaining caveats:

- native Windows builds remain untested
- the dataset helper scripts are shell scripts aimed at Unix-like environments
- macOS testing covers small graphs, not the full benchmark datasets

For the PCH/named-module refactor checks and six-/seven-vertex compilation
measurements on macOS and Ubuntu, see
[verification results](../tests/benchmarks/named-module-verification.md).
Run `python scripts/verify_platform.py` in the build environment to reproduce
both backend suites; on Ubuntu with upstream Clang installed, add
`--compiler clang++`. Run serially per source checkout because runtime code
generation shares `src/codegen_output/plan.cpp`.

### SIMD set operations

The normal runtime uses header-only NEON (ARM64) or CPU-checked AVX2 (GCC/Clang
x86) intersection and subtraction kernels for sufficiently large sorted candidate
sets, with scalar fallback. Count-only and output-producing operations share
the comparison logic. Profiling retains the original scalar-work counters.
See [implementation and validation](../tests/benchmarks/simd-set-operations.md)
for memory contracts, tests, and benchmark limitations.
The [subtraction follow-up](../tests/benchmarks/simd-subtraction.md) covers the
accumulated match masks, upper bounds, and extra vertex exclusion.

Search, removal, and index mapping also use shared array kernels. Index mapping
supports NEON/AVX2; small-set SIMD search remains a benchmark candidate.
See [sorted-set kernels and measurements](../tests/benchmarks/sorted-set-operations.md).

### Vertex-set workspace pools

Normal and profiling VertexSet share a dedicated header-only, fixed-capacity
worker pool. Compatible requests reuse buffers; larger requests cannot reuse
undersized buffers from earlier graphs. Owning sets retain their originating
pool through moves and temporary views. Generated entry points configure the
pool before starting workers; allocation accounting also belongs to the pool.
See [pool design and verification](../tests/benchmarks/vertex-set-pool.md), including
thread-confinement and cache-retention limitations.
VertexSet uses two pointers and two 32-bit fields (24 bytes on the supported
64-bit targets); sizes beyond UINT32_MAX are rejected rather than truncated.

MiniGraph's normal and profiling backends also share a variable-capacity pool
and move-only scratch container. See [MiniGraph storage design and tests](../tests/benchmarks/minigraph-pool.md).

### Tooling

1. CMake >= 3.20
2. A C++17 compiler (the Conda environment supplies the current platform compiler)
3. Python 3.14 in the supplied environment
4. `numpy` for the Python CSR API
5. `clang-format` optional
6. Installed CMake packages for oneTBB, fmt, cxxopts, and pybind11, plus OpenMP

## Tested Graph Data

1. Wiki
2. YouTube
3. Patents
4. LiveJournal
5. Orkut
6. Friendster
