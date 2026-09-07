# Diagnostic: -O3 versus -O3 -fno-inline

## Scope

The opt-in `GRAPHMINI_EXPERIMENTAL_NO_INLINE` switch adds `-fno-inline` only to
dynamic query compilation. `-O3` remains enabled. PCH and backend-module variants
are tested separately, with their own build directories and configuration-aware
query caches. Compiler command checks confirm the flags; Clang tracing is off.
No default was changed and no runtime implementation was moved out of headers.

This suppresses optional inlining broadly in the consumer, including eligible
standard-library/oneTBB calls. It is not a GraphMini-only or loop-depth-specific
policy; mandatory/always-inline calls may remain inline. Separately built host,
runtime, and oneTBB libraries retain their existing options. CMake supplies
compatible precompiled dependencies for each query configuration.

## Method

- Platforms: local macOS arm64 and Ubuntu 22.04 on Jupiter, in
  `/data/juelin/GraphMini`; Clang 21.1.8 and oneTBB 2023.1.0.
- Six cases: six-/seven-vertex cliques, stars (edge-IEP), and cycles, using
  GraphPi, nested TBB, and cost-model pruning.
- Compilation: three API cache misses/hits per case on a complete eight-vertex
  graph. Dependencies are warm. Every generated plan is checked against the
  symmetry-normalized exhaustive oracle. Ninja command times isolate the
  consumer compiler; API times include scheduling, code generation, building,
  copying, and loading. Ninja history compaction is outside the timed sample.
- Execution: a **separate process**, complete 14-vertex graph, one and two
  threads. Five batches per case/thread count, each at least three executions
  and targeting 100 ms wall time, capped at 128 executions. Reported time is
  the median of batch means of `RunResult.execution_time_seconds`.
  Graph construction, compilation, and initial warm-up are excluded.
  This timer includes the host's per-run thread-control/context setup. Because
  compilation and execution use different graph fixtures, their times must not
  be added to claim a measured end-to-end single-request latency.
- Runtime counts use independent closed forms for complete graphs: cliques
  `choose(n,k)`, stars `n*choose(n-1,k-1)`, cycles `perm(n,k)/(2*k)`. These formulas
  are also checked against exhaustive enumeration on the eight-vertex graph.
- The changed no-inline variants additionally run the 432-case scheduler,
  pruning, query-semantics, and threading oracle suite. CTest/compiler/lifetime
  tests run for all builds, including module/host interoperability where enabled.
- PCH retains C++17 and modules use C++20, but **baseline versus no-inline within
  each backend uses the same language standard**. Baseline/diagnostic order is
  reversed for the second backend. These are sequential, small-sample runs on
  non-dedicated machines, not a statistical performance guarantee.

The execution graph is synthetic and small, though larger than the oracle
fixture. Results establish a diagnostic tradeoff, not production-dataset
performance. Absolute timings should not be compared with earlier runs under
different host loads.

## Results

All four configurations built and passed CTest on both machines. Both no-inline
backends passed all 432 broad oracle checks on each platform: **1,728 executions,
zero mismatches**, in addition to the correctness checks embedded in every
compilation/execution measurement.

The initial six-case sweep showed a substantial compilation/execution tradeoff.
Jupiter's consumer compiler times decreased by approximately 7–66% across the
cases, but execution slowed. macOS timings were noticeably variable: some cases
had no compilation benefit or were slower. A flag-wide speedup is not guaranteed.

To check order/load effects, the seven-cycle was then repeated in **forward and
reverse variant order** on both platforms, reusing the existing builds. The table
below uses pooled medians from those confirmations: six compilation samples and
ten execution-batch means per configuration/thread count.

### Seven-cycle confirmation

API and compiler times are seconds; execution times are milliseconds.
Each cell shows **baseline -O3 → -O3 -fno-inline**.

| Platform/backend | Compiler command (s) | API cache miss (s) | Execution, 1 thread (ms) | Execution, 2 threads (ms) |
| --- | ---: | ---: | ---: | ---: |
| macOS / PCH | 1.102 → 0.312 | 1.284 → 0.446 | 4.59 → 12.85 | 3.36 → 9.43 |
| macOS / backend module | 0.877 → 0.326 | 1.086 → 0.542 | 4.59 → 13.07 | 3.24 → 9.54 |
| Jupiter / PCH | 1.299 → 0.462 | 1.486 → 0.650 | 8.02 → 24.28 | 5.44 → 17.31 |
| Jupiter / backend module | 1.442 → 0.509 | 1.689 → 0.763 | 7.79 → 26.38 | 5.94 → 19.01 |

For this confirmation, API cache misses became approximately **50–65% shorter**,
but execution became **2.8–3.4x slower**. The broad six-case sweep and alternating
confirmation are archived separately rather than blended into one result:
[macOS sweep](no-inline-macos.json), [Jupiter sweep](no-inline-ubuntu.json),
[alternating confirmation](no-inline-confirmation.json).

**Decision: keep normal -O3 as the default.** Disabling optional inlining is a
useful diagnostic and potentially useful for a future fast-compilation tier,
but it sacrifices too much execution performance to adopt unconditionally.
The next targeted experiment should preserve hot inner-loop computation while
moving cold/setup/outer-loop operations behind compiled calls. These results
support testing that idea; they do not prove selective inlining will preserve
execution speed or achieve the same compilation savings.

Implementation checkpoint: `6576e3a`. Driver command recognition and safe
resumption were corrected in later checkpoints without changing runtime code;
the archived run metadata records resumes. `286442e` added the alternating-order
confirmation. No default compiler flags or runtime algorithms were changed.

## Reproduction

In the dependency environment:

```sh
python scripts/run_inlining_experiment.py
# On Jupiter:
python scripts/run_inlining_experiment.py --compiler clang++
python scripts/summarize_inlining_experiment.py .verification/inlining
# After all four builds exist:
python scripts/confirm_inlining.py
```

The driver uses `build-inline-{pch,backend}` and
`build-noinline-{pch,backend}`, preserving generated source and temporarily
backed-up cache entries. `--resume` can continue completed stages; it rejects
committed implementation changes. Logs, commands, and full JSON results remain
under `.verification/inlining` on each machine. Nothing is pushed to GitHub.
