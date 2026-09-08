# Header-only SIMD set operations

The normal VertexSet backend now delegates intersection to
`src/backend/set_ops/set_ops.h`. Dedicated `scalar.h`, `neon.h`, and
`avx2.h` headers implement the kernels. Subtraction is unchanged.

## Contract and selection

Inputs are sorted, unique uint32_t IDs. The output must not overlap either input.
Inputs need no padding or vector alignment. Write kernels store exactly the
number of matches, including at the final partial block. Count-only kernels
do not allocate or write an output array. VertexSet retains pooled-buffer
ownership and the exclusive upper-bound semantics.

The block-comparison approach is inspired by LOVE's shuffle intersections:
https://github.com/juelinl/LOVE/blob/main/src/intersection_algos.cpp
and https://github.com/juelinl/LOVE/blob/main/src/vec_operation.cpp.
This implementation does not import LOVE's tables, bitmap representation,
aligned-access assumptions, or full-width output stores. It extracts matching
lanes with exact scalar stores; vectorized output compaction remains future work.

ARM64 uses four-lane NEON. GCC/Clang x86 uses eight-lane AVX2 only after a cached
CPU-support check; function-target attributes isolate AVX2 without enabling it
globally. Other targets retain scalar execution. The x86 AVX2 boundary can retain
a function call when the caller's target lacks AVX2. Header definitions permit,
but do not force, inlining.

Both input lengths must be at least 32 to select SIMD. This is a provisional
cutoff, not a tuned universal crossover. Short bounded intersections retain the
original scalar early-exit algorithm. Larger bounded inputs are restricted to
prefixes strictly below the upper bound before normal dispatch. There is no
galloping or bitmap dispatch in this checkpoint.

The profiling VertexSet backend shares the scalar implementation, including
consumed-input positions. Its counters retain their original scalar work
meaning; they are NOT SIMD instruction counts or SIMD execution timings.
Tests independently check bounded comparison/neighbor counter deltas.

The query-library build fingerprint includes all new set_ops headers, so edits
invalidate previously generated cached query libraries.

## Reproducing validation

In the existing project dependency environment:

```sh
cmake --build build-inline-pch -j 6
ctest --test-dir build-inline-pch --output-on-failure
PYTHONPATH=build-inline-pch/lib python tests/runtime_simd.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_large.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_smoke.py
build-inline-pch/bin/set_ops_benchmark
```

Run generated-query tests serially within a checkout: they temporarily replace
the shared generated plan and restore it on exit. Run each script in a separate
Python process. For the experimental full backend module, substitute
`build-inline-backend` after building that configuration.

The direct test covers 6,541 set pairs in both normal and profiling builds:
all pairs of subsets of a six-element universe, vector-width/cutoff boundaries,
unequal lengths, disjoint/identical/mixed sets, randomized overlap, high unsigned
IDs, null empty views, unaligned inputs, and exact-sized outputs. Direct SIMD
calls are tested even below the dispatch cutoff. Expected values come from
std::set_intersection, independently of the implementation.

`runtime_simd.py` adds 72 executions on K64 and a seeded 64-vertex dense graph
(triangle and induced/non-induced star), with adjacency lists large enough for
SIMD, three pruning modes, OpenMP/nested TBB, and one/two threads. The oracle
enumerates unordered triangles and center/unordered-leaf triples, counting each
occurrence once without relying on generated symmetry correction.
Existing suites provide 432 small-pattern and 48 six-/seven-vertex executions
against the independent symmetry-normalized exhaustive oracle.

Sanitizer command (Apple Clang on macOS; environment Clang on Ubuntu):

```sh
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc tests/set_ops_regression.cpp \
  -o /tmp/graphmini-set-ops-asan
/tmp/graphmini-set-ops-asan
```

On this macOS installation, Conda Clang's ASan runtime stalled during sanitizer
initialization before main. Apple Clang 21's ASan/UBSan successfully ran the test.
This does not affect the ordinary Conda Clang 21 Release build.

## Performance scope

Verified on macOS ARM64 and Jupiter Ubuntu 22.04 x86-64 at implementation
checkpoint `084587f5`, using the existing Clang 21 Release environments:

| Check | macOS | Jupiter |
| --- | --- | --- |
| PCH CTest | 5/5 | 5/5 |
| Backend-module CTest | 7/7 | 7/7 |
| PCH small-pattern oracle | 432/432 | 432/432 |
| PCH six-/seven-vertex oracle | 48/48 | 48/48 |
| PCH large-adjacency oracle | 72/72 | 72/72 |
| Backend-module large-adjacency oracle | 72/72 | 72/72 |
| Direct normal-kernel ASan + UBSan | pass | pass |

The 432/48 suites were not rerun under the module backend for this change;
module integration is covered by its CTest and large-adjacency suite.

The supplied microbenchmark compares the scalar kernel with public dispatch,
for count and write operations, sizes 8 through 1024, size ratios 1:1 and 1:16,
and disjoint or fully overlapping smaller inputs. It reports median nanoseconds
per call across five batches, with a compiler memory barrier preventing count
hoisting. It includes dispatch/call overhead but not VertexSet allocation.

Recorded timings are in `simd-set-operations.json`. Across the tested sizes
32–1024 (the SIMD-selected cases), count-only speedups were 1.52–3.30x on
macOS and 4.26–4.94x on Jupiter. Output-producing speedups were 1.22–2.51x
and 1.31–5.51x respectively. These ranges describe this particular run,
not guaranteed gains for arbitrary inputs. The sizes 8/16 remain scalar and
show dispatch overhead/noise rather than SIMD acceleration.

These are synthetic hot-cache results, not an end-to-end query speedup claim;
short-list overhead, overlap, size imbalance, and cache behavior all matter.
The cutoff and output packing should be tuned only with representative workloads.
