# Offline plans: IEP first, then bitmap

`3/` through `7/` index connected, unlabeled Graph Atlas patterns. Edge-induced
counting prefers array + IEP when outgoing scheduling supports it, otherwise
bitmap when eligible. Vertex-induced counting uses bitmap when eligible, never
IEP. Ordinary array-only plans remain on-demand and are listed as skipped.

Each `<schedule-sha256>/` contains `plan.json` and `iep.cpp` or `bitmap.cpp`. Scheduling is
outgoing-first; pruning is disabled; nested oneTBB and direct bitmap operations
are enabled for bitmap variants. Normal runtime array fallback remains available within a bitmap
kernel. Degree-based parallel thresholds are computed once at query entry.

Generated identifiers use `sN` for array-backed vertex sets and `bN` for bitmaps.
Both representations retain the same IR set ID. A single availability check
after BitGraph construction selects a bitmap-enabled or array-only continuation;
boundary conversions create ordinary scoped `bN` values when their inputs exist.

With the desired build's `lib` directory on `PYTHONPATH`:

```sh
python scripts/precompile_bitmap_plans.py --generate-only
python scripts/precompile_bitmap_plans.py --sizes 3 4 5 6 7
```

For a large catalog, populate the same cache using independent parallel builds
before running the second command to verify counts and record receipts:

```sh
cmake -S . -B build-macos-clang -DGRAPHMINI_PRECOMPILED_PLANS=ON
cmake --build build-macos-clang --target bitmap_catalog --parallel 4
```

Use your actual build directory on other platforms. Bulk compilation currently
supports the standard PCH build, not experimental modules or compiler flags.

The second command compiles eligible variants, checks counts on two small graphs
with one and two threads, and resumes through the build's existing kernel cache.
Use `--query-types edge` or `vertex` to restrict semantics, or `--no-prefer-iep`
for a forced bitmap-only comparison. Requires NumPy and
NetworkX. Run serially, without another runtime compilation in the same checkout.

Binaries stay in the selected build's ignored `python_plan_cache`; SHA-256 build
receipts are in ignored `.cache/bitmap-plans/`. They are platform/build-specific,
not portable binaries. `gm.precompile_offline_plan(adjacency, "edge")` returns a
cached compiled plan without requiring a data graph; use `plan.run(graph, ...)`
to search different graphs. Ordinary `compile_plan` uses the same catalog when
the configuration matches; scheduling still runs, but a hit skips code generation.
Explicit `compile_plan` options are not silently changed. The older
`precompile_bitmap_plan` / `describe_bitmap_plan` APIs force bitmap selection;
the new `precompile_offline_plan` / `describe_offline_plan` APIs prefer IEP by
default. This is a selection policy, not a measured guarantee of faster runtime.
Bulk builds follow `index.json`, so superseded bitmap sources can remain locally
without being compiled. The historical schema identifier is retained to keep
logical schedule IDs stable; strategy and source hashes identify active kernels.

Schedule IDs hash versioned scheduled topology, symmetry constraints and query
semantics. Kernel IDs additionally hash source and the native build fingerprint.
Original labels are metadata, not part of schedule identity. Source changes can
change kernel IDs without changing schedule IDs. Regenerate catalog metadata
after updating codegen; unchanged source can reuse its existing binary.

Python compilation APIs check the catalog immediately after outgoing scheduling
for no-pruning, nested-runtime plans. A hit requires matching options, counting
metadata, formatter mode, codegen fingerprint, source hash, and an existing
binary under the current runtime build ID. It skips IR construction, lowering,
code generation, and formatting; \`generated_code\` reads the saved artifact.
Other configurations and stale/missing entries follow normal compilation,
reusing the computed schedule. \`compilation_profile["schedule_cache_hit"]\`
distinguishes this fast path from an ordinary source-cache hit.
