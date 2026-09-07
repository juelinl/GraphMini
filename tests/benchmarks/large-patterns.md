# Six- and seven-vertex compilation measurements

Same Clang 21.1.8/macOS arm64 environment and optimization flags as the initial
baseline. Three compile/link samples per case; scheduling plus source generation
is averaged over 20 repetitions. These are exploratory wall-clock measurements,
with no OS-cache flushing or isolation from other machine activity.

| Pattern / backend / pruning | Scheduling + codegen | Compile with PCH | Compile without PCH | Link |
| --- | ---: | ---: | ---: | ---: |
| clique6 openmp none | 1.89 ms | 0.201 s | 1.336 s | 0.090 s |
| star6 openmp none | 0.62 ms | 0.220 s | 0.924 s | 0.095 s |
| cycle6 openmp none | 1.78 ms | 0.391 s | 1.234 s | 0.115 s |
| clique6 nested costmodel | 2.85 ms | 1.139 s | 2.270 s | 0.139 s |
| star6 nested costmodel | 0.91 ms | 0.349 s | 1.442 s | 0.106 s |
| cycle6 nested costmodel | 2.59 ms | 1.041 s | 2.137 s | 0.137 s |
| clique7 openmp none | 66.50 ms | 0.265 s | 1.486 s | 0.157 s |
| star7 openmp none | 5.91 ms | 0.714 s | 2.237 s | 0.148 s |
| cycle7 openmp none | 17.43 ms | 0.578 s | 1.562 s | 0.162 s |
| clique7 nested costmodel | 51.92 ms | 1.191 s | 2.102 s | 0.141 s |
| star7 nested costmodel | 4.63 ms | 0.604 s | 1.365 s | 0.119 s |
| cycle7 nested costmodel | 15.58 ms | 1.745 s | 3.075 s | 0.097 s |

All sizes denote total query vertices. Stars use edge-induced IEP; cliques and
cycles use ordinary edge-induced matching. All cases use the GraphPi scheduler
and the same synthetic graph metadata. OpenMP uses no pruning; nested TBB uses
cost-model pruning, so differences between those rows cannot be attributed to
parallelization alone.

A fresh PCH takes **1.346 s** (median). The PCH compile
column reuses it and excludes this one-time cost. All 72 C++ compilation commands
and 36 link commands succeeded. These benchmarks do not execute graph matching;
the existing compiler regression suite was rerun and passed separately.

## Observations

- PCH remains useful across all twelve cases, but its relative benefit varies
  with the generated query: dependency parsing is only part of compilation.
- Seven-vertex nested TBB cycles take the longest with PCH in this sample
  (1.745 s). The generated source is about 13 KB.
- Scheduling plus code generation for K7 is now 52–67 ms, up from 2–3 ms for K6.
  This combined metric does not isolate scheduler search from source writing.
- Source size alone does not predict compile time: IEP stars and nested TBB
  clique/cycle plans have different template and optimization workloads.
- Compare PCH/no-PCH within this run. Machine-load variability makes comparisons
  to the earlier four-vertex run less reliable.

## Reproduce

```sh
cmake --build build-conda --target compilation_benchmark
python scripts/benchmark_compilation.py --sizes 6 7 --repeats 3 \
  --output /tmp/graphmini-pch-large-patterns.json
```

Raw samples and generated-source byte counts are saved in
`pch-large-patterns-macos-arm64.json`. The harness accepts sizes four through
seven; it now includes cycles as well as stars and cliques. No production
compiler settings were changed.
