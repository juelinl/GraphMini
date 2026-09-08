# Fixed-capacity VertexSetPool refactor

The shared header `src/backend/vertex_set_pool.h` defines
`minigraph::internal::VertexSetPool`, used by both normal and profiling VertexSet
implementations. No generated-code or compiler-codegen changes are required.

## Fixed-size allocation API

A dedicated pool is configured at construction:

```cpp
std::atomic_uint64_t allocated{0};
minigraph::internal::VertexSetPool pool(4096, allocated);
auto* buffer = pool.acquire();
// Use up to 4096 uint32_t values.
pool.release(buffer);
```

Capacity is immutable. Every buffer in this pool has the same capacity, so
acquire/release do not perform per-buffer size lookup or size-class searching.
The free list reserves space before issuing a new allocation, making release
non-allocating. Newly allocated data is uninitialized, as in the old pool.
The accounting counter must remain valid while new allocations can occur.

## VertexSet integration

The pool-owned configuration/accounting follow-up passed 7/7 PCH and 9/9
module CTests on macOS and Jupiter, plus 144 graph-growth/shrink executions
per build on each platform (576 total). ASan/UBSan pool checks passed on both.
The module interoperability test additionally checks that C++17 host
configuration and allocation accounting are shared with C++20 module code.

The generated entry point calls internal::VertexSetPool::configure_for_graph()
before running worker loops. This validates the maximum degree and stores a
default capacity of max_degree + 1. VertexSet delegates storage selection to
for_request(), using max(constructor_request, default_capacity). Constructor
requests are honored even when larger than the configured graph maximum.

A thread-local registry caches the current compatible pool. Smaller requests
can reuse a larger fixed-capacity pool; requests beyond its capacity select or
create a larger pool. Existing pools never resize, so outstanding sets from an
earlier graph remain valid. This addresses the old undersized-buffer reuse risk
when a later graph increases the configured capacity.

Each owning VertexSet stores its originating pool pointer instead of a boolean.
Destruction returns its buffer to that pool, not whichever pool is currently
selected. Copies remain borrowed; moves, swaps, and rvalue bounded/remove views
transfer the pool pointer with ownership. Allocation accounting continues through
internal::VertexSetPool::TOTAL_ALLOCATED, a 64-bit atomic. It counts newly
allocated bytes since the last reset, not resident bytes: reusing a retained
buffer after a reset does not increment it.

The runtime-header fingerprint already includes the new top-level backend
header, invalidating stale query-library caches. This is an internal layout
change, not a promise of binary compatibility with previously built consumers.

## Concurrency and memory boundaries

- Pools and owning sets are thread-confined. Owners must be destroyed on the
  originating worker before that worker exits. Assertions check thread identity
  and outstanding ownership when enabled.
- Borrowed views may be read by other workers while their owner remains alive.
  Copying a set still does not extend the owner's lifetime.
- This refactor does not make concurrent independent queries safe. Existing
  graph-level globals, including the pool default capacity, remain; no new execution-context
  plumbing or scheduler semantics are introduced.
- Pools and cached buffers remain until worker exit. A sequence of increasing
  capacities may retain multiple pools. There is no idle-pool eviction policy
  in this change.
- Pool selection and an originating-pool pointer add some bookkeeping. No
  whole-query performance improvement is claimed from the extraction itself.

## How MiniGraph compares

MiniGraph already has a dedicated, non-nested MiniGraphPool class, currently
defined in `src/backend/minigraph.h` and duplicated in the profiling backend.
Its Get() returns a thread-local pool. ManagedContainer owns allocations and
returns them to that pool.

Unlike fixed-size VertexSet storage, MiniGraph storage is variable-capacity:
small requests use 4 KB buffers, larger buffers carry their capacities and are
reused by capacity. Resize grows storage and copies existing contents. That
design is useful for auxiliary adjacency storage whose required size varies.

MiniGraphPool was inspected but not changed or consolidated in this refactor.
Its thread/lifetime behavior is not strengthened by extracting VertexSetPool.

## Validation

Verified at checkpoint `ad701d92` on macOS ARM64 and Jupiter Ubuntu 22.04
x86-64 with the existing Clang 21.1.8 Release environments:

| Check | macOS | Jupiter |
| --- | --- | --- |
| PCH CTest | 7/7 | 7/7 |
| Backend-module CTest | 9/9 | 9/9 |
| PCH graph growth/shrink | 144/144 | 144/144 |
| PCH dense-graph SIMD oracle | 72/72 | 72/72 |
| PCH six-/seven-vertex oracle | 48/48 | 48/48 |
| PCH small-pattern oracle | 432/432 | 432/432 |
| Module graph growth/shrink | 144/144 | 144/144 |
| Module dense-graph SIMD oracle | 72/72 | 72/72 |
| Normal/profile pool ASan + UBSan | pass | pass |
| Set-operation ASan + UBSan | pass | pass |

Total: 1,824 graph executions with zero mismatches. The 432/48 suites were
not rerun under the module backend in this change. Sanitizer checks used
Apple Clang locally and environment Clang on Jupiter.

New direct tests in `tests/vertex_set_pool.cpp`, built for normal and profiling
backends, cover fixed-capacity reuse/accounting, invalid zero capacity, live
owners across graph growth, shrinking, oversized constructor requests, moves,
borrowed copies/assignments, temporary views, and independent worker lifetimes.

`tests/runtime_pool.py` compiles plans on K4, then runs those same plans on
K4, K64, K8, K128, K5, and K64 in one process: 144 executions per backend.
It covers induced triangles, edge-induced IEP stars, and induced stars;
OpenMP/nested TBB; no/cost-model pruning; and one/two threads. Expected counts
are symmetry-free complete-graph formulas.

The module interoperability test now constructs an owning VertexSet in the
C++20 module consumer and checks it and a borrowed copy from the C++17 host.

Reproduction in the existing dependency environment (serially per checkout):

```sh
cmake --build build-inline-pch -j 6
ctest --test-dir build-inline-pch --output-on-failure
PYTHONPATH=build-inline-pch/lib python tests/runtime_pool.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_simd.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_large.py
PYTHONPATH=build-inline-pch/lib python tests/runtime_smoke.py
cmake --build build-inline-backend -j 6
ctest --test-dir build-inline-backend --output-on-failure
PYTHONPATH=build-inline-backend/lib python tests/runtime_pool.py
PYTHONPATH=build-inline-backend/lib python tests/runtime_simd.py
```

Each Python script restores the shared generated source. Pool growth tests
intentionally keep different graph executions in the same process.

## Compact VertexSet layout

Follow-up implementation checkpoint: `c384d9a`.

Both normal and profiling VertexSet now store their fields in this order:

```cpp
internal::VertexSetPool* m_pool;
IdType* m_data;
IdType m_size;
IdType m_vid;
```

On the supported 64-bit targets this reduces sizeof(VertexSet) from 32 to 24
bytes: two 8-byte pointers followed by two 4-byte fields, with no padding gap.
The regression test statically checks the 24-byte size when pointers are 8 bytes.
This is a 25% reduction in object storage, not a claimed execution-time speedup.

An ID's width alone does not guarantee that every conceivable set count fits:
the set of all 2^32 possible uint32_t IDs would have an unrepresentable count.
VertexSet now explicitly limits its stored size to UINT32_MAX. The borrowed-view
constructor and set_size() reject out-of-range values with length_error instead
of narrowing silently. Owning construction likewise validates its capacity
request before allocating. configure_for_graph() validates the graph maximum
degree before changing the default; rejection preserves the previous setting.
Pool padding and allocation-byte arithmetic remain wide.

The public size() return type remains uint64_t for source compatibility.
Internal set-operation outputs are subsets of their left inputs, so valid input
sizes also bound the stored output counts. Counters and graph metadata are not
narrowed by this change.

Boundary tests use metadata-only synthetic views to accept UINT32_MAX and reject
UINT32_MAX + 1 without allocating huge arrays; a rejected set_size() leaves the
previous size unchanged. Normal/profile pool tests, existing set-operation
tests, and the C++17-host/C++20-module interoperability test cover the new layout.
Existing binaries must be rebuilt; the runtime-header cache fingerprint changes
automatically. No codegen changes are needed.

The compact layout was reverified on macOS and Jupiter: 7/7 PCH CTests,
9/9 module CTests, size-boundary ASan/UBSan checks, and all 1,824 graph
executions listed above passed with zero mismatches. Both normal and profiling
objects satisfy the 24-byte assertion. Local sanitizer checks additionally
covered the profiling pool and all 6,557 set-operation pairs.
