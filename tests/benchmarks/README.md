# Runtime compilation baseline

See [six- and seven-vertex measurements](large-patterns.md) for the larger-pattern
follow-up. The current harness also includes cycles and accepts `--sizes 6 7`.

The first baseline uses macOS 26.6.2, Apple A18 Pro, Conda Clang 21.1.8,
C++17, `-O3`, and the existing `plan_module` compile/link flags. Results are
exploratory: three repetitions, not a controlled performance guarantee.

| Generated plan | C++ with reusable PCH | C++ without PCH | Linking |
| --- | ---: | ---: | ---: |
| K4 / OpenMP / no pruning | 0.246 s | 1.415 s | 0.157 s |
| Three-leaf star IEP / OpenMP / no pruning | 0.315 s | 1.402 s | 0.153 s |
| K4 / nested TBB / cost-model pruning | 0.881 s | 2.221 s | 0.197 s |
| Three-leaf star IEP / nested TBB / cost-model pruning | 0.489 s | 1.783 s | 0.159 s |

Values are medians. Generating a fresh PCH takes 1.350 s (median), in addition
to compilation and linking for an artifact-cold build. Scheduling and C++ source
generation average 0.038–0.124 ms over 20 repetitions for these small patterns;
this includes existing logging, but excludes writing generated files. Larger
patterns may have substantially higher scheduling cost.

## Reproduce

Activate the build's Conda environment, configure with
`-DGRAPHMINI_BUILD_TESTS=ON`, then run from the repository root:

```sh
cmake --build build-conda --target compilation_benchmark
python scripts/benchmark_compilation.py --build-dir build-conda \
  --repeats 3 --output /tmp/graphmini-pch-baseline.json
```

The harness currently supports the macOS Clang/Ninja build. It reuses actual
Ninja commands but redirects all outputs into a fresh temporary directory. It
does not change tracked plans, replace build artifacts, or clear module caches.
Generated-source compilation alternates PCH/no-PCH order between repetitions.
Every measurement invokes the compiler; none is a finished-module cache hit.

"Cold" means generating the PCH artifact, not flushing the OS filesystem cache.
These stage timings exclude CMake/Ninja invocation, shared-library copying,
dynamic loading, Python overhead, and query execution. The raw samples are in
`pch-baseline-macos-arm64.json`; notably the TBB samples have substantial variance.

## Implication for the modules experiment

PCH already removes most dependency parsing overhead in these examples. A C++20
module prototype should therefore be compared against **reused PCH**, not just
textual headers. Measure both module construction and reuse, keep identical
optimization/backend settings, and separately check result correctness. No
module migration or production compile-path change is included in this baseline.
