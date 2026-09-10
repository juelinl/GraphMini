"""Build a resumable, IEP-first / bitmap-next catalog of Graph Atlas patterns.

Run serially against one GraphMini build (compile_plan shares a source file).
Requires networkx and numpy in the selected GraphMini Python environment.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import time

import graphmini as gm
import networkx as nx
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "graphmini-outgoing-bitmap-v1"


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def adjacency(graph):
    return "".join("1" if graph.has_edge(i, j) else "0"
                   for i in range(len(graph)) for j in range(len(graph)))


def csr_graph(graph):
    rows = [sorted(graph.neighbors(v)) for v in range(len(graph))]
    return gm.Graph.from_csr(np.array([0] + list(np.cumsum([len(r) for r in rows])), dtype=np.uint64),
                             np.array([v for row in rows for v in row], dtype=np.uint32))


def schedule_identity(description, query_type):
    # Input vertex IDs / matching_order are deliberately not part of identity.
    return dict(schema=SCHEMA, scheduler="outgoing", semantics=query_type,
                adjacency=description["adjacency"],
                restrictions=sorted(map(list, description["restrictions"])))


def verify(plan, graph, query_type):
    automorphisms = sum(1 for _ in nx.isomorphism.GraphMatcher(graph, graph).isomorphisms_iter())
    n = len(graph)
    complete_count = math.factorial(n + 1) // automorphisms
    if query_type == "vertex" and graph.number_of_edges() != n * (n - 1) // 2:
        complete_count = 0
    checks = []
    for name, host, expected in [("pattern", graph, 1),
                                 ("complete", nx.complete_graph(n + 1), complete_count)]:
        data = csr_graph(host)
        for threads in (1, 2):
            actual = plan.run(data, num_threads=threads).number_of_matches
            if actual != expected:
                raise RuntimeError(f"{query_type}: {name}/{threads}: {actual} != {expected}")
            checks.append(dict(host=name, threads=threads, count=actual))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "plans")
    parser.add_argument("--sizes", nargs="+", type=int, choices=range(3, 8), default=list(range(3, 8)))
    parser.add_argument("--query-types", nargs="+", choices=("edge", "vertex"), default=["edge", "vertex"])
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--no-prefer-iep", action="store_true", help="Force the original bitmap-only policy")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Exclude simultaneous catalog writers; unrelated runtime compilation must
    # also stay idle, as with the existing compile_plan API.
    lock = output / ".catalog.lock"
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    source_path = ROOT / "src/codegen_output/plan.cpp"
    original = source_path.read_bytes()
    try:
        for size in sorted(set(args.sizes)):
            entries = []
            index_path = output / str(size) / "index.json"
            previous = json.loads(index_path.read_text())["entries"] if index_path.exists() else []
            retained = [e for e in previous if e["query_type"] not in args.query_types]
            for atlas_id, graph in enumerate(nx.graph_atlas_g()):
                if len(graph) != size or not nx.is_connected(graph):
                    continue
                query = adjacency(graph)
                for query_type in sorted(set(args.query_types)):
                    description = gm.describe_offline_plan(query, query_type, prefer_iep=not args.no_prefer_iep)
                    identity = schedule_identity(description, query_type)
                    schedule_id = digest(identity)
                    entry = dict(atlas_id=atlas_id, query_type=query_type, schedule_id=schedule_id,
                                 bitmap_selected=description["bitmap_selected"],
                                 bitmap_reason=description["bitmap_reason"], strategy=description["strategy"],
                                 iep_width=description["iep_width"], execution_query_type=description["execution_query_type"])
                    if description["source"]:
                        directory = output / str(size) / schedule_id
                        directory.mkdir(parents=True, exist_ok=True)
                        source = description["source"]
                        source_hash = hashlib.sha256(source.encode()).hexdigest()
                        source_file = description["strategy"] + ".cpp"
                        (directory / source_file).write_text(source)
                        metadata = dict(identity, schedule_id=schedule_id, source_sha256=source_hash,
                                        atlas_id=atlas_id, input_adjacency=query,
                                        matching_order=description["matching_order"],
                                        bitmap_reason=description["bitmap_reason"],
                                        source_file=source_file, strategy=description["strategy"],
                                        iep_width=description["iep_width"],
                                        execution_query_type=description["execution_query_type"],
                                        codegen_build_id=description["codegen_build_id"],
                                        schedule_counting=description["schedule_counting"],
                                        format_enabled=description["format_enabled"],
                                        config=dict(pruning="none", parallel="nested_rt", bitmap=description["bitmap_selected"],
                                                    bitmap_direct=description["bitmap_selected"], bitmap_deferred_counts=False))
                        write_json(directory / "plan.json", metadata)
                        entry["source_sha256"] = source_hash
                        entry["source_file"] = source_file
                        if not args.generate_only:
                            start = time.perf_counter()
                            plan = gm.precompile_offline_plan(query, query_type, prefer_iep=not args.no_prefer_iep)
                            if plan.generated_code != source:
                                raise RuntimeError("Offline and compile API generated different source")
                            checks = verify(plan, graph, query_type)
                            kernel_identity = dict(schedule_id=schedule_id, source_sha256=source_hash,
                                                   build_id=gm.plan_build_id)
                            kernel_id = digest(kernel_identity)
                            module = Path(plan.module_path)
                            record = dict(kernel_identity, kernel_id=kernel_id, module_path=str(module),
                                          strategy=description["strategy"], atlas_id=atlas_id, query_type=query_type,
                                          binary_sha256=hashlib.sha256(module.read_bytes()).hexdigest(),
                                          compilation=dict(plan.compilation_profile), checks=checks,
                                          preparation_and_checks_seconds=time.perf_counter() - start)
                            # Machine-specific binaries and receipts never enter the source catalog.
                            write_json(ROOT / ".cache/bitmap-plans" / gm.plan_build_id / (kernel_id + ".json"), record)
                            print(f"READY n={size} atlas={atlas_id} {query_type}/{description['strategy']} {kernel_id[:12]}", flush=True)
                            del plan
                    entries.append(entry)
                    combined = sorted(retained + entries, key=lambda e: (e["atlas_id"], e["query_type"]))
                    write_json(index_path, dict(schema=SCHEMA, entries=combined))
            bitmaps = sum(e["strategy"] == "bitmap" for e in entries)
            ieps = sum(e["strategy"] == "iep" for e in entries)
            print(f"SIZE {size}: {ieps} IEP, {bitmaps} bitmap / {len(entries)} variants; "
                  f"{'generated' if args.generate_only else 'compiled and checked'}", flush=True)
    finally:
        source_path.write_bytes(original)
        lock.unlink()


if __name__ == "__main__":
    main()
