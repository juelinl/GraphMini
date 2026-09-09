# Experimental Metal clique backend

Opt-in C++ API (`Backend::count_cliques`) and CLI for exact K3–K8 counting on
sorted, simple undirected graphs. CPU-built BitGraphs are batched into shared
Metal buffers; GPU edge-prefix tasks share rows and keep independent DFS state.
64-bit counts are checked for overflow. No general-pattern codegen, MiniGraph,
GPU array fallback, or IEP support yet; existing CPU defaults are unchanged.

## Build and verify

Requires macOS, an Apple7-or-newer GPU, Xcode, and its Metal toolchain.
Tested on A18 Pro/macOS 26.6.2.
The standalone build requires only CMake and the Apple toolchain, not conda/TBB.

```sh
xcodebuild -downloadComponent MetalToolchain
cmake -S src/backend/metal -B build-metal -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++
cmake --build build-metal
ctest --test-dir build-metal --output-on-failure
MTL_SHADER_VALIDATION=1 build-metal/graphmini_metal_test
```

Alternatively enable `-DGRAPHMINI_BUILD_METAL=ON` in the normal project build.

## Count

Input: `vertex_count edge_count` on the first line, then one undirected edge
`u v` per line, with zero-based IDs. The CLI sorts and deduplicates edges.

```sh
build-metal/graphmini_metal_cli graph.txt 6 3
```

Reports one warmup and three measured runs as JSON lines. `total_ms` includes
graph validation, CPU construction/packing, buffer allocation/copy, GPU execution,
waiting and CPU reduction. Graph loading and pipeline compilation are excluded;
stderr reports process elapsed time including setup. `gpu_ms` is only device
execution time. Turn GPU validation off for timings.

Optional arguments after repeats: batch size in MiB (default 64), maximum tasks
per batch (default 16384). C++ options also bound scratch memory (default 64 MiB).
Oversized regions fail explicitly; no work is silently omitted. A region spanning
multiple batches is uploaded per batch. Small universes use fixed-depth scalar
mask kernels, larger ones use device scratch. No speedup is guaranteed.

Reuse a `Backend` session to cache pipelines; calls on one session must be
serialized. This build-tree API locates its `.metallib` in the build directory;
installation/packaging and Python `compile_plan` integration are not implemented.
