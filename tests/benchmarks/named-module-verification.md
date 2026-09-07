# Named-module refactor verification

## Scope and decision

The dynamic-query backend can now import the official oneTBB 2023.1.0 `tbb`
module. PCH remains the default; named modules are opt-in. Static and profiling
plans continue to use PCH. This is not a module conversion of the entire backend
or the C++ standard library.

The compatibility workaround guards two memory-resource exports in a generated
build-directory copy of `tbb.cppm`. Neither the installed oneTBB headers nor its
source checkout is modified.

## Correctness changes

- Fixed a pooled-buffer lifetime error in chained `VertexSet::remove()` and
  `bounded()` operations. Rvalue operations now preserve ownership of a reused
  buffer. Both normal and profiling implementations have direct regression tests.
  The previously failing seven-cycle case now returns the oracle's 30 matches,
  rather than 43.
- Made the IR ordering comparator lexicographic and added an anti-symmetry test.
- Fingerprinted runtime headers and build configuration in dynamic-library cache
  paths, so old query libraries are not reused after these changes.
- Runtime test scripts restore the generated plan source on normal exit. They
  must still run serially within one source checkout.

The runtime oracle enumerates injective embeddings and divides by the complete
pattern automorphism count. It checks actual counts, not just agreement between
PCH and named-module results.

## Platforms and correctness results

Verified on 2026-09-07 at code checkpoint
`e195705afeccb3623441a617345c7648dfc2712b`:

| Platform | CPU | Query compiler | Dependencies |
| --- | --- | --- | --- |
| macOS 26.6.2 arm64 | Apple A18 Pro | Conda Clang 21.1.8 / libc++ | Python 3.14.7, CMake 4.4.3, oneTBB 2023.1.0 |
| Ubuntu 22.04.5 x86-64, Jupiter | 2 x Xeon Silver 4214R | Conda Clang 21.1.8 / libstdc++ | Python 3.14.7, CMake 4.4.3, oneTBB 2023.1.0 |

Both platforms passed full builds and all CTest tests: compiler regressions,
normal/profile temporary-lifetime tests, and the named-module smoke test when
enabled. On **each platform and each backend**, runtime checks passed:

- 432 small-pattern executions: vertex, edge, and edge-IEP semantics; all three
  schedulers; three pruning modes; OpenMP and nested TBB; one and two threads.
- 48 larger-pattern executions: six-/seven-vertex cliques, stars, and cycles on
  two eight-vertex graphs; OpenMP/no pruning and nested TBB/cost-model pruning;
  one and two threads.

All 1,920 runtime executions had zero oracle mismatches. The recorded rows also
match exactly across both platforms and both compilation backends. Jupiter's
loaded shared-library dependencies resolve to the project-local oneTBB install,
whose runtime version reports `2023.1.0`.

## Compilation results

Seconds below are medians of three samples. The representative consumer is
`cycle7_nested_costmodel`; the ratio range covers all 12 six-/seven-vertex cases.

| Measurement | macOS | Ubuntu |
| --- | ---: | ---: |
| Build backend PCH | 4.337 | 2.815 |
| Build TBB module BMI | 4.382 | 3.179 |
| Build TBB module object | 0.153 | 0.051 |
| Consumer compile, textual headers | 2.040 | 3.797 |
| Consumer compile, PCH | 1.183 | 1.531 |
| Consumer compile, named module | 1.967 | 2.030 |
| Link, PCH | 0.080 | 0.106 |
| Link, named module | 0.077 | 0.099 |
| Build/load proxy, PCH | 1.460 | 1.676 |
| Build/load proxy, named module | 2.132 | 2.144 |
| Named/PCH consumer compilation ratio, all cases | 1.56–5.47x | 1.33–3.02x |

PCH was faster for every measured consumer on both platforms. The named-module
path does not justify replacing the default. These are small-sample timings on
non-dedicated machines, not a statistical performance guarantee or a controlled
comparison between the two CPUs. Consult the individual samples before drawing
conclusions about small differences.

Raw data: [macOS](named-module-macos.json), [Ubuntu](named-module-ubuntu.json),
and [verification summary](named-module-checks.json).

## Reproduction

Use the dependencies in `environment.yml`, plus upstream Clang and its matching
`clang-scan-deps` tool for named modules (the Ubuntu environment added
`clangxx=21` and `clang-tools=21` from conda-forge). CMake >=3.28 and Ninja are
required for this experimental path. Install the project-local oneTBB release
and run:

```sh
python scripts/install_onetbb.py
python scripts/verify_platform.py --compiler clang++
```

On macOS, omit `--compiler` when reusing a build already configured with the
Conda compiler. The driver builds both variants, runs CTest and both runtime
suites, then benchmarks six- and seven-vertex patterns. Logs and machine-readable
results are written under `.verification/`.

For the Ubuntu verification, an isolated checkout and environment were created
through `ssh jupiter` under `/data/juelin/graphmini-verification`. Existing server
projects, environments, and system packages were left unchanged.

## Measurement boundaries

Each benchmark uses three repetitions, rotating consumer compilation order.
PCH, textual includes, and named-module consumers use matched C++20 flags and
the same oneTBB version within each machine. The dependency artifacts are
regenerated for dependency-build timings, but OS filesystem caches are not
flushed. Warm consumer timings reuse those artifacts.

The build/load sample uses a minimal CMake/Ninja target with the measured compile
and link commands, plus shared-library loading. It is a build-driver proxy, not
an end-to-end timing of Python `compile_plan`; API startup, source writes,
configuration, and cache-copy overhead are excluded. These measurements do not
establish graph-execution performance or performance on large real datasets.

The two reusable artifacts cover different amounts of code: the PCH contains
GraphMini's backend headers, whereas the named module contains oneTBB and the
consumer still parses GraphMini's headers. This is a comparison of the available
implementation choices, not a general conclusion that C++ modules are slower.
Precompiling more of the GraphMini backend would be a separate experiment and
should retain this correctness/measurement gate before changing the default.
