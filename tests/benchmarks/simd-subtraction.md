# SIMD subtraction follow-up

Implementation checkpoint: `dd1286e8`.

The normal VertexSet backend now uses header-only SIMD for both `subtract()`
and `subtract_cnt()`, including their upper-bounded overloads. The kernels are
in the existing `src/backend/set_ops/{scalar,neon,avx2,set_ops}.h` files.

## Reusing intersection correctly

Intersection and difference now share the NEON/AVX2 block comparison and
lane-mask helpers. Difference is not implemented by complementing the mask
from each block pair: a left block can match several successive right blocks.

For each left block, difference accumulates a pending match mask. When the
right block's maximum reaches or exceeds the left block's maximum, no later
right block can match it (inputs are sorted and unique). Only then does the
kernel emit or count the complement, additionally excluding `other.m_vid`.
If the SIMD loop ends first, the pending mask is carried into scalar tail
handling so earlier matches cannot reappear in the result.

The contract is `A minus B minus {excluded}`. The excluded ID is explicit and
mandatory, including UINT32_MAX; that value does not disable exclusion.
Unsigned comparisons preserve ordering above INT32_MAX.

Like intersection, SIMD uses a provisional minimum of 32 elements in both
inputs. Short bounded calls retain a scalar early-exit loop; larger calls
restrict both arrays to prefixes strictly below the upper bound before dispatch.
NEON uses four lanes on ARM64; CPU-checked, function-targeted AVX2 uses eight
on supported GCC/Clang x86 builds. No global AVX2 flag or forced inlining is added.

Output stores write exactly the surviving values and need no padding or
alignment. Count-only operations allocate and write nothing. Inputs and outputs
must not alias. VertexSet allocation and pooled-buffer ownership are unchanged.

The profiling backend's subtraction implementation is deliberately unchanged:
its existing scalar-work counters are not hardware SIMD counters. Both profiling
and normal wrappers are checked against the same independent result oracle.

## Tests

`tests/set_ops_regression.cpp` now checks intersection and subtraction on
6,557 set pairs. Difference expected results use std::set_difference followed
by independent filtering of the excluded ID and upper bound. Coverage includes:

- all pairs of subsets of a six-element universe;
- vector-width/cutoff boundaries and strongly unequal sizes;
- randomized overlaps and high unsigned IDs;
- empty/null views, unaligned inputs, and exact-sized output allocations;
- exclusions present in A only, both sets, or neither, including UINT32_MAX;
- retained left-block masks across multiple right blocks and scalar tails;
- direct SIMD count/write kernels, public dispatch, and all VertexSet overloads.

Direct SIMD tests run even below the public cutoff. For profiling wrappers,
the excluded ID must fit the profiler fixture's vertex-ID range; direct kernel
tests still exercise the full uint32_t range.

Run serially per checkout in the dependency environment:

```sh
cmake --build build-inline-pch -j 6
ctest --test-dir build-inline-pch --output-on-failure
PYTHONPATH=build-inline-pch/lib python tests/runtime_simd.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_large.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_smoke.py
```

For module compatibility, build `build-inline-backend`, run its CTest suite,
and rerun runtime_simd.py with that build's lib directory on PYTHONPATH.
Each runtime script restores the shared generated source on exit. They run in
separate processes, avoiding cross-test workspace-pool growth.

The dense-graph suite includes induced stars, which exercise subtraction in
generated code. Its oracle counts unordered leaf triples directly, without
using generated symmetry correction.

ASan/UBSan runs use Apple Clang locally and environment Clang on Jupiter:

```sh
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc tests/set_ops_regression.cpp \
  -o /tmp/graphmini-difference-asan
/tmp/graphmini-difference-asan
```

## Verification results

Verification completed on macOS ARM64 (Apple A18 Pro) and Jupiter Ubuntu 22.04
x86-64 (Xeon Silver 4214R), using the existing Clang 21.1.8 Release builds:

| Check | macOS | Jupiter |
| --- | --- | --- |
| PCH CTest | 5/5 | 5/5 |
| Backend-module CTest | 7/7 | 7/7 |
| PCH small-pattern oracle | 432/432 | 432/432 |
| PCH six-/seven-vertex oracle | 48/48 | 48/48 |
| PCH large-adjacency oracle | 72/72 | 72/72 |
| Backend-module large-adjacency oracle | 72/72 | 72/72 |
| Direct normal-kernel ASan + UBSan | pass | pass |

The 432/48 suites were not rerun under the module backend for this change.
The profiling-wrapper sanitizer run also passed locally.

## Benchmark scope

`build-inline-pch/bin/set_ops_benchmark` now reports intersection and difference,
both count-only and output-producing. Difference uses the preexisting scalar
merge algorithm's comparisons and excludes UINT32_MAX (absent in these
benchmark inputs). The allocated output can hold the entire left input.

These are synthetic hot-cache kernel measurements, not whole-query speedups.
The benchmark uses sizes 8–1024, ratios 1:1 and 1:16, full/no overlap, and median
timing across five batches. Small inputs remain scalar. Representative runtime
benchmarks and dispatch tuning remain future work.

Recorded per-case measurements are in `simd-subtraction.json`, including
intersection measurements after extracting the shared helpers. The SIMD-selected
subtraction cases generally improved, but not universally: the local 256-element
identical-set count case measured 164.8 ns scalar versus 214.8 ns dispatched.
This is why the provisional cutoff is not a promise of a speedup for every
input. Output density and timing variability also affect the results.
