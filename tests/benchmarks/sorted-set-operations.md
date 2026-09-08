# Shared sorted-set kernels

`src/backend/set_ops/sorted.h` now supplies array-only search, single-element
removal, and left-index intersection kernels. Both VertexSet backends delegate
bounded/bounded_cnt, remove/remove_cnt, and indices to these helpers. MiniGraph's
get_indices uses the same indices_write dispatcher. Wrapper allocation, borrowed
views, vertex metadata, and rvalue ownership transfer stay outside set_ops.
No codegen changes or additional profiling counters are introduced.

## Contracts and dispatch

- Arrays contain sorted unique uint32_t IDs. Indices in the left array must fit
  uint32_t. Counts and cursor arithmetic use size_t.
- Empty arrays may be null; no padding or alignment is required. Output must not
  alias an input and needs only as many elements as the result, including zero.
- bounded is a strict prefix (`id < upper`). A shared linear search handles at
  most 64 elements; larger inputs retain binary search. prefix_size also shares
  the binary kernel while preserving its previous policy.
- remove_cnt searches once and subtracts one on equality. remove searches once
  in the wrapper and copies the two ranges with memcpy only if an ID is present.
  Its absent-ID borrowed-view behavior and temporary ownership are unchanged.
- indices_write emits positions in the left input, not matching vertex IDs.
  Identical input pointers get a consecutive-index fast path. A right input
  smaller than 1/50 of the left uses monotonic binary searches, including safe
  handling of absent IDs. The ratio check avoids multiplication overflow.
- Otherwise, inputs of at least 32 elements each use four-lane NEON or eight-lane
  AVX2 match masks; smaller/unsupported cases use a scalar merge. x86 dispatch
  checks AVX2 availability without enabling AVX2 globally. Scalar tails retain
  the left base offset when emitting positions.
- SIMD lower-bound scanning is implemented and tested as a benchmark candidate,
  but not enabled in lower_bound_index: gains depend on where the search stops.
- MiniGraph's cost-model cursor advancement also uses a shared helper. Its
  end-of-range guard precedes dereferencing, unlike the previous helper. Both
  cost-model build paths also check for an exhausted cursor before reading it;
  this fixes a pre-existing out-of-range read on empty/exhausted iterator subsets.

The old duplicated MiniGraph binary_search helper was removed. The runtime
header fingerprint covers sorted.h, so generated query caches are invalidated.

## Verification and measurement

`tests/sorted_ops.cpp` checks 4,882 pairs against independent standard-library
oracles: exhaustive small sets, random larger sets, SIMD boundaries, high
unsigned IDs, sparse/absent matches, exact-size unaligned buffers, identity
mapping, and wrapper ownership. It runs against normal and profiling runtimes.
Direct SIMD entry points are exercised even below the dispatcher threshold.
Existing MiniGraph parent/child rebuild and VertexSet lifetime tests also apply.
Targeted cost-model parent/child cases additionally exercise empty, early-ending,
and last-vertex iterator subsets, checking every returned adjacency.

On macOS ARM64 and Ubuntu x86-64 (Jupiter), all 11 PCH and 13 backend-module
CTests passed. Normal-backend ASan/UBSan checks passed on both platforms.
Each platform passed 912 generated-query executions: PCH runtime_pool (144),
runtime_simd (72), runtime_large (48, including 6/7-vertex patterns), runtime_smoke
(432), plus module runtime_pool (144) and runtime_simd (72). All 1,824 matched
independent expected counts. After adding the final exhausted-cursor guards,
both builds were rebuilt and CTests plus runtime_pool repeated on both platforms.

`tests/sorted_ops_benchmark.cpp` measures hot-cache kernels at -O3. Each result is
the median of five batches of 50,000 calls after 1,000 warm-up calls, with a
compiler memory barrier and consumed results. Search shapes are front (0),
middle (1), past-end (2), and varying (3). Index shapes are disjoint (0),
identical values in separate arrays (1), and partial overlap (2). This initial
benchmark uses equal-sized index inputs; it does not characterize skewed graph
workloads or establish an optimal dispatcher threshold. These measurements are
not whole-query speedup claims.

Raw results are recorded in [sorted-set-operations.json](sorted-set-operations.json).
For the tested index inputs of at least 32 elements, scalar/dispatch ratios
ranged from 0.94–2.62x on macOS NEON and 1.42–6.10x on Jupiter AVX2. Thus the
NEON dispatcher had a small regression in one case; SIMD is not universally
faster. At 128 elements with identical values in separate arrays, scalar versus
dispatch was 626.5 vs 294.4 ns locally and 428.0 vs 224.0 ns on Jupiter.

Search results depend strongly on stop position: at 64 elements, a front hit
took 1.25 ns scalar versus 2.22 ns direct SIMD locally, while a past-end search
took 67.2 versus 21.7 ns. For larger sets, binary search avoids scanning the full
prefix. We retain the existing production search policy and leave SIMD scanning
as an explicit candidate. Short-call timings also depend on compiler inlining,
code layout, and CPU frequency; the scalar and dispatch columns can differ even
when they ultimately select the same algorithm. Threshold tuning needs broader
workload measurements rather than treating these microbenchmarks as universal.
