# Where query compilation time goes

Profiled on 2026-09-07 on macOS arm64 and Ubuntu 22.04/Jupiter, using Clang
21.1.8, oneTBB 2023.1.0, GraphPi, nested TBB, and cost-model pruning. Queries are
six-/seven-vertex cliques, stars (edge-IEP), and cycles on the complete eight-vertex
graph. Each case has three warm-dependency cache misses and three cache hits.
All generated plans were executed and checked against the symmetry-normalized
oracle. CTest/compiler/lifetime tests passed on both platforms, including the
backend module's C++17-host interoperability test.

## Main findings

1. **Actual API timings include scheduling.** The compiler reads already-stored
   graph statistics, invokes GraphPi, constructs/optimizes its IR, and emits C++
   before looking up the generated-library cache. Graph construction and initial
   statistics preparation are outside these measurements.
2. Scheduling represented approximately **0.1–5.0% of cache-miss API time** across
   the measured cases. Seven-clique scheduling was more expensive than
   seven-cycle scheduling: about 31–32 ms locally and 52 ms on Jupiter.
   These bounds do not generalize to larger patterns or different graph stats.
3. The dominant cache-miss cost is **C++ compilation, primarily LLVM
   optimization and machine-code generation**, not C++ source emission or
   GraphPi scheduling.
4. The backend module adds a measurable dependency scan/collation step: about
   **69 ms locally and 60 ms on Jupiter** for the seven-cycle. This explains part,
   not all, of the module/PCH difference; consumer compilation also differs.
5. Cache hits still run scheduling and code generation. A schedule/plan cache
   would primarily help repeated API hits; it is not the largest opportunity for
   the cache misses measured here.

## Seven-cycle breakdown

All values are milliseconds, medians of three samples with Clang tracing enabled.
The PCH API uses its operational C++17 configuration; modules use C++20.

| Stage | macOS PCH | macOS module | Jupiter PCH | Jupiter module |
| --- | ---: | ---: | ---: | ---: |
| API total, cache miss | 966 | 1,096 | 1,527 | 1,785 |
| GraphPi scheduling | 14.6 | 14.9 | 29.7 | 30.4 |
| IR construction / auxiliary planning / C++ emission | <0.2 | <0.2 | <0.2 | <0.2 |
| Build subprocess, total | 952 | 1,080 | 1,496 | 1,754 |
| ↳ Build-system file checks | 16 | 17 | 19 | 17 |
| ↳ Module scan + dependency collation | 0 | 69 | 0 | 60 |
| ↳ Consumer compiler command | 848 | 900 | 1,342 | 1,532 |
| ↳ Link command | 62 | 64 | 108 | 107 |
| ↳ Build time not assigned to logged commands | 25 | 31 | 31 | 38 |
| Cache library copy + load | 0.26 | 0.27 | 0.37 | 0.38 |
| API total, cache hit | 14.4 | 15.2 | 28.9 | 29.8 |

Rows marked with arrows are inside the build total. Medians are calculated
independently and need not sum exactly. The residual includes process/driver
startup, gaps, and unlogged work; it is **not** a separately measured CMake-only
timer. Ninja command durations have millisecond resolution and multi-output
edges are deduplicated. The collector rejects unexpected dependency rebuilds.

Clang's breakdown of the same consumer compilation:

| Clang stage | macOS PCH | macOS module | Jupiter PCH | Jupiter module |
| --- | ---: | ---: | ---: | ---: |
| Frontend | 50 | 76 | 98 | 138 |
| LLVM backend, total | 752 | 771 | 1,197 | 1,345 |
| ↳ Optimizer | 387 | 403 | 645 | 710 |
| ↳ Code-generation passes | 364 | 368 | 551 | 641 |
| Inliner pass (inside optimization) | 31 | 34 | 44 | 48 |

These are nested trace totals, not additive independent costs. The inliner pass
alone is not the dominant expense. In one local seven-cycle trace, the largest
per-function backend entries belong to generated `Loop0`/`Loop1`/`Loop2`/`Loop3`
task bodies. Selective inlining or reducing duplicated generated bodies is worth
testing because it may reduce work throughout optimization/code generation—not
because simply disabling the inliner would eliminate the whole backend cost.
Execution performance must still be measured before adopting that change.

## Measurement safeguards and limits

- Instrumentation checkpoint: `37af521`; collector fix: `49e1765`. The final
  complete runs use the latter checkpoint. No inlining or scheduling-algorithm
  change was made for this experiment.
- Compiler stages are captured per `CompiledPlan` using scoped, thread-local
  telemetry; compiler decisions do not depend on the telemetry. Nested parent
  timings are labeled explicitly. The summarizer checks parent/child timing
  consistency and confirms cache hits never invoke the build.
- Each measured miss reuses built dependencies and temporarily removes only
  its generated cache library. The original library and generated source are
  restored. Ninja history is compacted **before timing**, preventing automatic
  log rewriting from invalidating command attribution. That preflight work is
  excluded; these are not cold-dependency or cold-filesystem measurements.
- Three-sample untraced API controls were also collected. Seven-cycle medians
  were 850/1,031 ms (macOS PCH/module) and 1,512/1,721 ms (Jupiter PCH/module).
  Controls use normal log maintenance; traced runs compact beforehand. Tracing,
  host load, warm-up, and sequential ordering can perturb results. Do not treat
  the difference as an exact tracing-overhead correction or use these small
  samples to establish statistical significance.
- Output is redirected to log files. Schedule diagnostics are measured
  separately from the scheduler call. The very short IR/emission stages are
  close to instrumentation overhead and are not useful micro-optimization targets.
- Raw traces are retained under `.verification/profiles/{pch,backend}` locally
  and `/data/juelin/GraphMini/.verification/profiles/{pch,backend}` on Jupiter.
  They include full Clang pass/function events. Committed summaries retain
  per-sample stage timings: [macOS](compilation-profile-macos.json),
  [Jupiter](compilation-profile-ubuntu.json).

## Reproduction and API access

In the dependency environment, run serially per checkout:

```sh
python scripts/run_compilation_profile.py
# Jupiter / upstream Clang:
python scripts/run_compilation_profile.py --compiler clang++
python scripts/summarize_compilation_profile.py .verification/profiles
```

This uses separate `build-profile-pch` and `build-profile-backend` directories.
For a single build, enable `GRAPHMINI_PROFILE_QUERY_COMPILATION=ON` to obtain
Clang JSON traces, then use `scripts/profile_compilation.py`. Regular builds do
not enable Clang tracing. API stage telemetry is available on every compiled plan:

```python
plan = gm.compile_plan(graph, query, "edge", scheduler="graphpi")
print(plan.compilation_profile["cache_hit"])
print(plan.compilation_profile["seconds"])
```
