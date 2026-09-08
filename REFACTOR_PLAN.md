# Remaining refactor and verification plan

1. Integrate the official oneTBB named module into opt-in dynamic query builds.
   Keep default PCH builds unchanged; isolate caches and reject conflicting modes.
2. Diagnose and fix the existing seven-cycle overcount. Require symmetry-normalized
   oracle agreement, not merely agreement between compilation backends.
3. Run compiler, small-pattern, and six-/seven-vertex correctness tests on macOS
   for PCH and named-module builds. Make the known-failing test pass genuinely.
4. Measure compilation and build/load costs for both backends using oneTBB
   2023.1.0, including first-build versus reusable dependency costs. Keep the
   default based on measurements, not on module availability alone.
5. Verify the exact checkpoint on Ubuntu through `ssh jupiter`, using an isolated
   checkout and user-local dependencies. Build and run both backend test suites;
   address portability failures without changing unrelated server projects.
6. Document results, limitations, and reproducible commands; checkpoint locally.
   Do not push or deploy. Named modules remain experimental unless evidence
   supports making them the default.

Progress:

- Official named module integrated into opt-in dynamic query builds.
- Seven-cycle overcount fixed (temporary-buffer lifetime); direct tests cover
  both normal and profiling runtimes.
- Strict ordering corrected in IR sorting, with a regression test.
- Runtime-header/build fingerprints added to the query-library cache.
- macOS and Ubuntu full builds and CTest suites pass for both backends.
- Each platform/backend passes 432 small-pattern and 48 six-/seven-vertex runtime
  checks against the symmetry-normalized oracle: 1,920 executions in total,
  zero mismatches. Recorded counts also match across platforms/backends.
- Jupiter verification uses Ubuntu 22.04 x86-64 and an isolated environment at
  `/data/juelin/graphmini-verification/env`; no system packages changed.
- `scripts/verify_platform.py` reproduces both builds, correctness suites, and
  matched C++20 compilation benchmarks with oneTBB 2023.1.0.
- Both compilation benchmarks are complete: named-module consumers were
  1.56–5.47x slower than PCH on macOS and 1.33–3.02x slower on Ubuntu across
  the 12 cases. PCH remains the default; named modules remain opt-in.
- Results, limitations, and reproduction commands are recorded in
  `tests/benchmarks/named-module-verification.md`, with raw samples alongside it.

All six steps above are complete. Further backend/standard-library module work
is a separate experiment, not a prerequisite for this checkpoint. No changes
have been pushed to GitHub.

## Follow-up: precompile the complete backend

Completed the opt-in `graphmini.backend` module prototype. It exports the stable
runtime interface with preserved C++17-host type identity and re-exports `tbb`.
Both platforms pass the C++17 interoperability test and all 1,920 canonical
PCH/backend-module oracle executions. The shared generated source and benchmark
cache entries are restored after tests.

Matched C++20 consumer compilation is near PCH on macOS and faster in this
Ubuntu run, but the actual seven-cycle `compile_plan()` cache miss remains
slower with modules. PCH therefore remains the default. Full results and raw
samples are in `tests/benchmarks/backend-module-verification.md`.

Any next performance phase should profile the real API's module scanning/build
driver overhead. Neither broader module exports nor faster direct compilation
alone should be used as the criterion for changing defaults.

## Follow-up: profile before selective inlining

Completed stage-level API, Ninja, and Clang profiling on macOS and Jupiter for
six-/seven-vertex clique/star/cycle queries, with cache misses/hits and untraced
controls. See `tests/benchmarks/compilation-profile.md` and its raw sample
summaries. The dominant measured cost is LLVM optimization/code generation;
GraphPi scheduling accounts for about 0.1–5% of cache misses in these cases.
Module scanning/collation adds roughly 60–70 ms for the seven-cycle.

No selective-inlining change has been made. A subsequent experiment should
target generated task-body complexity/cold runtime code, measure actual API
latency, and verify graph-execution performance as well as correctness.

## Follow-up: broad no-inline diagnostic

Completed `-O3` versus `-O3 -fno-inline` for PCH and backend modules on macOS and
Jupiter, with six-/seven-vertex compilation and synthetic execution measurements.
Both no-inline variants passed the broad oracle suite on both machines (1,728
checks). A forward/reverse-order seven-cycle confirmation reduced API cache-miss
time by roughly 50–65% while slowing execution by 2.8–3.4x.

Normal `-O3` remains the default. The experimental flag is available, but no
selective-inlining or out-of-line implementation split has been made yet.
Results and limitations: `tests/benchmarks/no-inline-experiment.md`.

## Follow-up: SIMD set operations

Compilation-time experiments are paused. Added header-only scalar, NEON, and
AVX2 intersection kernels behind the existing VertexSet API. The normal runtime
dispatches to SIMD for sufficiently large inputs; the profiling runtime shares
the scalar kernel to preserve its work counters. Subtraction and compiler
code generation are unchanged. See `tests/benchmarks/simd-set-operations.md`.

Next candidates are representative end-to-end runtime benchmarks, dispatch
threshold tuning, and subtraction kernels. Bitmap MiniGraphs and galloping
remain separate experiments rather than implicit parts of this change.

### Subtraction follow-up

Added SIMD subtraction and subtraction counts using the same NEON/AVX2 block
comparison helpers as intersection. Masks accumulate until the left block can
be finalized and survive scalar-tail handling. Upper bounds, other.m_vid
exclusion, exact-size stores, and the profiling backend's scalar counters are
preserved. See `tests/benchmarks/simd-subtraction.md` for validation and measurements.

## Dedicated VertexSetPool

Extracted a shared fixed-capacity thread-local pool implementation. Owning sets
retain the originating pool pointer; constructor capacity requests are honored,
and larger graphs select compatible storage without invalidating older owners.
No codegen changes. MiniGraph already has a separate variable-capacity pool
and remains unchanged. See `tests/benchmarks/vertex-set-pool.md`.
