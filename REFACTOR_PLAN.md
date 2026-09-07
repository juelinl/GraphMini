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
  both normal and profiling runtimes. All 48 larger PCH checks pass the oracle.
- Strict ordering corrected in IR sorting, with a regression test.
- Runtime-header/build fingerprints added to the query-library cache.
- Full macOS named-module correctness checks are in progress.
- Jupiter access confirmed: Ubuntu 22.04 x86-64. Isolated dependency environment
  created at `/data/juelin/graphmini-verification/env`; no system packages changed.
