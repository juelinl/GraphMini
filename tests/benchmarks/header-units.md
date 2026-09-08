# Experimental C++20 header units

Historical report: the seven-cycle mismatch recorded below has since been fixed
by retaining pooled-buffer ownership across chained temporary set operations.
The current large-pattern suite is expected to pass the oracle. The build cache
now also fingerprints runtime headers and compiler/build settings.

The recorded experiment below used oneTBB 2023.0.0. The later dependency upgrade
to 2023.1.0 supplies an official `tbb.cppm`, but these results do not measure it.

The opt-in build imports the existing `src/backend/backend.h` dependency tree
as a C++20 **header unit**. It is not a named-module rewrite and does not use
`import std`. The default remains C++17 with PCH.

Only the dynamic `plan_module` target uses C++20/imports. Static plans, profiling,
the scheduler/compiler, and the Python host retain their existing build path.
Clang still labels header-unit support experimental; this prototype is gated to
upstream Clang with Ninja and has been tested on Conda Clang 21.1.8/macOS arm64.

## Build and check

In the GraphMini Conda environment, use a separate build directory:

```sh
cmake -S . -B build-header-units -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHMINI_BUILD_TESTS=ON -DGRAPHMINI_EXPERIMENTAL_HEADER_UNITS=ON
cmake --build build-header-units --target graphmini plan_module compiler_regression
ctest --test-dir build-header-units --output-on-failure
PYTHONPATH=build-header-units/lib python tests/runtime_smoke.py
PYTHONPATH=build-header-units/lib python tests/runtime_large.py
```

The final command currently exposes a pre-existing counting issue described
below; it is intentionally not marked as a passing oracle test.

`scripts/build_header_unit.py` reads the consumer's entry from CMake's compile
database so the BMI inherits its architecture, optimization, macro, include,
and OpenMP flags. Ninja tracks the header unit's transitive header dependencies
through a depfile and rebuilds the consumer when the BMI changes. Generated query
source is unchanged; an experimental wrapper includes it and `plan.h` selects
`import` instead of `#include`.

Finished Python libraries use separate cache directories for PCH and header-unit
builds. As with the existing cache, other compiler/header changes can still
require a fresh build/cache; this is not a general cache-invalidation redesign.
Both build variants write the same generated `plan.cpp`, so do not run runtime
compilation tests from them concurrently.

## Correctness

- All 1,800 compiler configurations passed in the experimental build.
- All 432 small-pattern runtime executions matched the symmetry-normalized
  exhaustive oracle with header-unit imports.
- For six-/seven-vertex cliques, stars, and cycles on two eight-vertex graphs,
  all 48 results matched the PCH backend; 44 matched the oracle.
- Both backends return **43 instead of 30** for the seven-vertex edge-induced
  cycle on the seeded non-complete graph, with OpenMP/TBB and one/two threads.
  This discrepancy was reproduced on the existing PCH path and is not fixed by
  this experiment. It must not be interpreted as a passing correctness check.
  Independent deduplication by mapped edge sets also confirms 30 unique cycles.

`tests/runtime_large.py --results FILE` saves all results before reporting any
oracle failure. `--baseline FILE` compares backend behavior separately from
correctness, just as the small-pattern test does.

## Matched timing comparison

```sh
python scripts/benchmark_compilation.py --build-dir build-conda --sizes 6 7 \
  --header-units --repeats 3 --driver-case cycle7_nested_costmodel \
  --output /tmp/graphmini-header-unit-benchmark.json
```

The harness compares textual headers, PCH, and header units under identical
C++20/`-O3` flags, rotating their measurement order. It separately measures PCH
and BMI construction. All outputs are scratch artifacts; existing plans and
caches are untouched.

The optional driver case measures a fresh compile/link through a minimal
CMake/Ninja target plus `dlopen` time in a fresh Python process. It reuses the PCH
or BMI. This excludes CMake configuration, Python startup, query scheduling,
source-file writing, and finished-library copying; it is a build-plus-load proxy,
**not** the full `compile_plan` API latency.

## Results on this machine

Three samples per case, medians below. Negative change favors imports.

| Case | Reused PCH | Reused header unit | Compile-time change |
| --- | ---: | ---: | ---: |
| clique6 openmp none | 0.426 s | 0.369 s | -13.2% |
| star6 openmp none | 0.396 s | 0.366 s | -7.6% |
| cycle6 openmp none | 0.610 s | 0.561 s | -8.1% |
| clique6 nested costmodel | 1.545 s | 1.885 s | 22.0% |
| star6 nested costmodel | 0.644 s | 0.612 s | -5.0% |
| cycle6 nested costmodel | 1.608 s | 1.522 s | -5.3% |
| clique7 openmp none | 0.296 s | 0.330 s | 11.6% |
| star7 openmp none | 0.660 s | 0.541 s | -18.0% |
| cycle7 openmp none | 0.647 s | 0.506 s | -21.8% |
| clique7 nested costmodel | 1.343 s | 1.238 s | -7.8% |
| star7 nested costmodel | 0.805 s | 0.741 s | -7.9% |
| cycle7 nested costmodel | 1.790 s | 2.345 s | 31.0% |

Fresh dependency construction: **4.136 s PCH**, **3.823 s header unit**.
These are C++20 measurements from this run, not directly comparable to the
earlier C++17 run under potentially different machine load.

The seven-vertex TBB cycle's build-plus-load proxy had medians of **1.904 s**
for PCH and **3.576 s** for imports. Individual totals ranged from 1.843–2.661 s
and 2.165–4.838 s respectively. Driver samples were collected in blocks by
backend, unlike the rotating stage measurements. Variability is substantial;
these three-sample measurements do not establish a stable speedup or slowdown.

Imports had lower compile medians in nine of twelve cases, generally by about
5–22%, but regressed in three, including the largest cycle/TBB plan (31%).
**Keep PCH as the default.** This header-unit prototype does not demonstrate a
consistent enough advantage to justify migration. A named-module redesign is a
separate experiment, not something these measurements establish as faster.

Raw timing samples are in `header-units-macos-arm64.json`. The larger-pattern
counting discrepancy should be addressed before relying on those query results.
