"""Native schedule-cache hit/miss regression; run serially on a populated catalog."""
import contextlib
import itertools
import json
import os
from pathlib import Path
import sys

import graphmini as gm
import networkx as nx

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from precompile_bitmap_plans import adjacency, csr_graph
from runtime_test_support import restore_generated_plan_at_exit


@contextlib.contextmanager
def edited(path, content):
    original = path.read_bytes()
    try:
        path.write_text(content)
        yield
    finally:
        path.write_bytes(original)


@contextlib.contextmanager
def hidden(path):
    backup = path.with_name(path.name + ".schedule-cache-test-backup")
    assert not backup.exists()
    path.rename(backup)
    try:
        yield
    finally:
        # A binary miss may have recreated the artifact; restore the original.
        os.replace(backup, path)


def assert_hit(plan):
    profile = plan.compilation_profile
    assert profile["cache_hit"] and profile["schedule_cache_hit"], profile
    assert "scheduling" in profile["seconds"]
    for stage in ("base_ir", "planning_total", "codegen_total", "execution_lowering", "cpp_emission",
                  "clang_format", "source_write", "build"):
        assert stage not in profile["seconds"], (stage, profile)


def assert_source_hit(plan):
    profile = plan.compilation_profile
    assert profile["cache_hit"] and not profile["schedule_cache_hit"], profile
    assert "codegen_total" in profile["seconds"] and "build" not in profile["seconds"]


def main():
    restore_generated_plan_at_exit()
    query_graph = nx.complete_graph(5)
    query = adjacency(query_graph)
    host = csr_graph(query_graph)
    for semantics in ("vertex", "edge"):
        plan = gm.precompile_offline_plan(query, semantics)
        assert_hit(plan)
        expected_source = gm.describe_offline_plan(query, semantics)["source"]
        assert plan.generated_code == expected_source
        for order in itertools.islice(itertools.permutations(range(5)), 12):
            relabeled = "".join(query[order[i]*5+order[j]] for i in range(5) for j in range(5))
            cached = gm.compile_plan(host, relabeled, semantics, pruning_type="none",
                                     parallel_type="nested_rt", scheduler="outgoing", bitmap=True, bitmap_direct=True)
            assert_hit(cached)
            assert cached.module_path == plan.module_path and cached.generated_code == expected_source
            for threads in (1, 4):
                assert cached.run(host, num_threads=threads).number_of_matches == 1

    # A non-clique relabeling checks actual identity invariance, not just K5 labels.
    query_graph.remove_edge(3, 4)
    query = adjacency(query_graph)
    host = csr_graph(query_graph)
    plan = gm.precompile_bitmap_plan(query, "vertex")
    assert_hit(plan)
    expected_source = plan.generated_code
    for order in itertools.islice(itertools.permutations(range(5)), 12):
        relabeled = "".join(query[order[i]*5+order[j]] for i in range(5) for j in range(5))
        cached = gm.precompile_bitmap_plan(relabeled, "vertex")
        assert_hit(cached)
        assert cached.module_path == plan.module_path
        assert cached.run(host, num_threads=4).number_of_matches == 1

    entries = json.loads((ROOT / "plans/5/index.json").read_text())["entries"]
    entry = next(e for e in entries if e.get("source_sha256") == Path(plan.module_path).stem
                 and e["query_type"] == "vertex")
    directory = ROOT / "plans/5" / entry["schedule_id"]
    manifest, source = directory / "plan.json", directory / entry["source_file"]
    metadata = json.loads(manifest.read_text())
    corruptions = ["not JSON"]
    for field, value in (
        ("codegen_build_id", "stale"), ("schedule_id", "0" * 64),
        ("execution_query_type", "edge"), ("source_sha256", "../invalid"),
        ("source_file", "../wrong.cpp"), ("format_enabled", False),
        ("config", dict(metadata["config"], bitmap=False)),
        ("schedule_counting", dict(metadata["schedule_counting"], redundancy=-1)),
    ):
        corruptions.append(json.dumps(dict(metadata, **{field: value})))
    for content in corruptions:
        with edited(manifest, content):
            cached = gm.precompile_bitmap_plan(query, "vertex")
            assert_source_hit(cached)
            assert cached.generated_code == expected_source
            assert cached.run(host, num_threads=4).number_of_matches == 1
    with hidden(manifest):
        assert_source_hit(gm.precompile_bitmap_plan(query, "vertex"))
    with hidden(source):
        assert_source_hit(gm.precompile_bitmap_plan(query, "vertex"))
    with edited(source, expected_source + "\n// stale artifact\n"):
        assert_source_hit(gm.precompile_bitmap_plan(query, "vertex"))

    # An explicit option mismatch must not silently select catalog defaults.
    different = gm.compile_plan(host, query, "vertex", scheduler="outgoing", pruning_type="none",
                                parallel_type="nested_rt", bitmap=True, bitmap_direct=False)
    assert not different.compilation_profile["schedule_cache_hit"]
    assert different.run(host, num_threads=4).number_of_matches == 1
    previous = os.environ.get("GRAPHMINI_FORMAT_CODE")
    try:
        os.environ["GRAPHMINI_FORMAT_CODE"] = "0"
        unformatted = gm.precompile_bitmap_plan(query, "vertex")
        assert not unformatted.compilation_profile["schedule_cache_hit"]
        assert "clang_format" not in unformatted.compilation_profile["seconds"]
        assert unformatted.run(host, num_threads=4).number_of_matches == 1
    finally:
        if previous is None:
            os.environ.pop("GRAPHMINI_FORMAT_CODE", None)
        else:
            os.environ["GRAPHMINI_FORMAT_CODE"] = previous

    # Missing native artifact: compile normally, then recover the old file.
    with hidden(Path(plan.module_path)):
        rebuilt = gm.precompile_bitmap_plan(query, "vertex")
        profile = rebuilt.compilation_profile
        assert not profile["cache_hit"] and not profile["schedule_cache_hit"]
        assert profile["seconds"]["build"] > 0
        assert rebuilt.run(host, num_threads=4).number_of_matches == 1
    assert_hit(gm.precompile_bitmap_plan(query, "vertex"))

    # IEP-first and explicit IEP resolve the same binary after a single schedule.
    entry = next(e for e in json.loads((ROOT / "plans/4/index.json").read_text())["entries"]
                 if e["strategy"] == "iep")
    metadata = json.loads((ROOT / "plans/4" / entry["schedule_id"] / "plan.json").read_text())
    query = metadata["input_adjacency"]
    iep = gm.precompile_offline_plan(query, "edge")
    explicit = gm.compile_plan(csr_graph(nx.complete_graph(6)), query, "edge_iep",
                               scheduler="outgoing", pruning_type="none", parallel_type="nested_rt")
    assert_hit(iep)
    assert_hit(explicit)
    assert iep.module_path == explicit.module_path
    print("PASS: early hits, no lowering/emission/formatting, relabeling, counts, IEP, "
          "stale/corrupt/missing artifacts, config isolation and native compilation on miss")


if __name__ == "__main__":
    main()
