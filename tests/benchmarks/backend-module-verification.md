# Full-backend module experiment

## Implementation

`src/backend/graphmini_backend.cppm` precompiles the stable backend and exports
the runtime types used by generated queries. It re-exports the official `tbb`
module. Queries select this path through `GRAPHMINI_USE_BACKEND_MODULE` in
`plan.h`; the user-facing CMake switch is
`GRAPHMINI_EXPERIMENTAL_BACKEND_MODULE=ON`.

The interface uses a global module fragment and exported using-declarations,
following oneTBB's compatibility approach. This preserves the identity of the
header-defined `Graph`, `Context`, and other types shared with the C++17 Python
host. A dedicated test links a C++17 host object against an importing C++20
consumer and checks live data access. Attaching these shared types directly to
a named module would change their C++ identity.

This is an ABI-preserving bridge, not a wholesale rewrite into module-attached
types or an out-of-line conversion of every runtime method. Inline/template
definitions remain available for optimization. Static and profiling plans
retain PCH, and the standard library has not been converted to modules.
The initial backend BMI still parses textual oneTBB headers to preserve the
existing header implementation, then re-exports the official module; dependency
construction therefore includes both module builds. Generated queries do not
reparse the backend headers.

## Reproduction

Use upstream Clang with matching `clang-scan-deps`, Ninja, CMake >=3.28, and the
project's other dependencies, including oneTBB 2023.1.0:

```sh
python scripts/verify_platform.py --backend-module \
  --module-build build-backend-module --output-dir .verification/backend
```

On Ubuntu, add `--compiler clang++`. This builds PCH and full-backend variants,
runs compiler/lifetime/interoperability tests and symmetry-normalized runtime
oracles, measures actual API cache misses/hits, and benchmarks six-/seven-vertex
query compilation with PCH, textual headers, oneTBB-only modules, and full-backend
modules. Run serially per checkout; generated plan source is restored on normal
exit. Cache libraries temporarily moved by the API benchmark are restored too.

Jupiter work uses `/data/juelin/GraphMini`, reusing the isolated environment at
`/data/juelin/graphmini-verification/env` and a symlink to the previously verified
project-local oneTBB installation. No system packages or upstream sources were
changed, and nothing was pushed to GitHub.

## Measurement boundaries

Direct compiler timings use matched C++20 flags and three repetitions on each
machine. Reusable PCH/BMI construction is measured separately; filesystem caches
are not flushed. The CMake build/load proxy is retained for comparison.

`benchmark_compile_api.py` additionally times the actual `compile_plan()` API:
scheduling, generation, cache lookup, and (on misses) build, cache copy, and
library load. Build dependencies are warm. Process startup, graph construction,
and execution are excluded from those timings; each resulting plan is executed
after timing and checked against the exhaustive oracle. The operational PCH API
uses its default C++17 build, whereas the module API uses C++20. The initial call
has unspecified query-cache state and is not treated as a cold-build measurement.

These are small-sample measurements on non-dedicated machines, not statistical
guarantees. No large real-world graph execution benchmark was performed.

## Correctness and provenance

Verified on 2026-09-07 with Clang 21.1.8, Python 3.14.7, CMake 4.4.3, and oneTBB
2023.1.0: macOS 26.6.2 arm64 (Apple A18 Pro, libc++) and Jupiter's Ubuntu 22.04.5
x86-64 (Xeon Silver 4214R, libstdc++).

Both PCH and backend-module builds passed all compiler and temporary-lifetime
tests on both machines. The module build additionally passed oneTBB's module
smoke test and the C++17 host interoperability test. Each platform/backend
passed 432 small-pattern and 48 six-/seven-vertex runtime executions against the
symmetry-normalized exhaustive oracle, with zero mismatches: 1,920 canonical
suite executions in total. The API benchmark separately checks its generated
seven-cycle plans against the oracle (2,880 matches in the complete eight-vertex
graph).

Runtime/CMake implementation checkpoint: `8b48959`. Verification began at that
checkpoint; `8536ca0` added documentation/ignore rules and placed API benchmark
backups on the cache filesystem before those measurements ran. Neither changed
runtime/compiler code. Raw verification metadata records the starting commit.

All recorded runtime rows also match exactly across platforms and backends.

## Results and decision

Seconds are medians of three samples. Consumer/API rows use the seven-cycle,
nested-TBB/cost-model case; ratio ranges cover all 12 six-/seven-vertex cases.

| Measurement | macOS PCH | macOS backend module | Ubuntu PCH | Ubuntu backend module |
| --- | ---: | ---: | ---: | ---: |
| Reusable dependency construction (see below) | 0.973 | 2.534 | 2.796 | 7.071 |
| Direct consumer compilation, C++20 | 0.879 | 0.894 | 1.518 | 1.498 |
| Consumer link | 0.064 | 0.065 | 0.106 | 0.105 |
| Actual API cache miss, dependencies warm | 0.970 | 1.086 | 1.512 | 1.845 |
| Actual API cache hit | 0.0147 | 0.0161 | 0.0331 | 0.0326 |

Module dependency construction is the **sum of medians** for the TBB BMI/object
and backend BMI/object builds, not an end-to-end cold CMake build measurement.
Individual samples and build/load proxy timings are in the raw data.

Across the 12 cases, backend-module/PCH direct compilation ratios ranged from
**0.988–1.064 on macOS** and **0.715–0.989 on Ubuntu**. Thus broader precompilation
largely closes the macOS gap and reduces direct compiler time on Ubuntu in this
run. In the same run, the oneTBB-only seven-cycle consumer took 1.355 s on macOS
and 1.992 s on Ubuntu, versus 0.894 s and 1.498 s with the complete backend module.
Do not compare absolute times against previous runs on differently loaded hosts.

However, the actual seven-cycle API cache miss was approximately 12% slower on
macOS and 22% slower on Ubuntu with the module path. **Keep PCH as the default**;
the module remains an opt-in prototype. A useful next investigation is to
attribute module scanning/build-driver costs inside the real API before further
interface restructuring. Direct compiler improvements alone do not establish
end-to-end improvement, and this experiment does not measure graph-execution
speed on production datasets.

Raw data and verification summaries: [macOS](backend-module-macos.json),
[Ubuntu/Jupiter](backend-module-ubuntu.json).
