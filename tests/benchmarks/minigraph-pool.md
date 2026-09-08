# Shared MiniGraph workspace storage

Both runtimes now include the same two header-only implementations:

- `src/backend/minigraph_pool.h`: raw Container handles and MiniGraphPool.
- `src/backend/managed_container.h`: move-only owning scratch storage.

Public names and generated calls are unchanged. Profiling instrumentation and
the existing differences in MiniGraph pruning policies are not consolidated.
The runtime-header fingerprint covers both new headers; rebuild consumers
because ManagedContainer now also holds its originating pool pointer.

## Allocation and lifetime

The pool remains thread-local and variable-capacity. Requests up to 1024
uint32_t elements use 4 KB buffers. Larger requests reuse the smallest available
adequate large buffer; new allocations grow approximately 1.5x and round to a
page, using checked integer arithmetic. There is no buffer eviction.

Buffers have unique ownership inside the pool. Raw Container values are
non-owning handles and must be returned exactly once. Pools cannot be copied.
Free-list capacity is reserved before acquisition, so returns do not allocate
or throw. Debug assertions check worker identity and outstanding ownership at
pool destruction. Accounting is still a shared 64-bit atomic of newly allocated
bytes since reset, not a resident-memory metric.

ManagedContainer records its originating pool and transfers that pointer with
its buffer during moves/swaps. Owners must remain on their worker and be
destroyed before its pool. Raw pointers and adjacency VertexSet views are
borrowed; growth or owner destruction invalidates them. This does not enable
concurrent independent queries or cross-worker owner migration.

## Preserved scratch semantics and safety fixes

- Indexing is capacity-based, not size-based. MiniGraph writes adjacency scratch
  data without maintaining a logical size for that buffer.
- Resize grows only and preserves the full previous capacity using memcpy.
  Smaller requests are no-ops. The old raw-pool Resize could copy past a smaller
  replacement allocation.
- Reserve may discard contents on growth, but now allocates the replacement
  before returning the old buffer. A failed allocation leaves the owner intact.
  Logical size stays unchanged for both Reserve and Resize.
- set_size rejects values larger than capacity; empty end() avoids null-pointer
  arithmetic. Empty construction with an explicit zero size still gets a small
  workspace, preserving the old behavior.

No whole-query performance improvement is claimed. ManagedContainer grows by
one pointer; the benefit here is shared implementation and explicit ownership.

## Tests

`tests/minigraph_pool.cpp` is built against both normal and profiling runtimes.
It checks small/large reuse, best-fit selection, accounting, full-capacity data
preservation, smaller Resize, invalid sizes, rejection without ownership loss,
moves, default/empty containers, and independent workers.

It also rebuilds Eager, Lazy, Online, and CostModel MiniGraphs across graph sizes
32, 128, 8, 256, and 16, with bounded/unbounded adjacency and parent-derived
children. Every adjacency is compared to independently enumerated expected IDs.
The module smoke test passes grown ManagedContainer storage to the C++17 host.

Standalone pool/container ASan+UBSan checks use GRAPHMINI_POOL_STANDALONE;
the regular tests additionally cover the MiniGraph classes and profiling hooks.
