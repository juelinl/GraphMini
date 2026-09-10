"""Native offline-cache and relabeling checks; run serially with PYTHONPATH set."""
import hashlib
import itertools
from pathlib import Path
import sys

import graphmini as gm
import networkx as nx

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from precompile_bitmap_plans import adjacency, csr_graph, digest, schedule_identity, verify
from runtime_test_support import restore_generated_plan_at_exit


def main():
    restore_generated_plan_at_exit()
    graph = nx.complete_graph(5)
    graph.remove_edge(3, 4)
    query = adjacency(graph)
    for kind in ("edge", "vertex"):
        description = gm.describe_bitmap_plan(query, kind)
        identity = schedule_identity(description, kind)
        assert description["bitmap_selected"]
        # Every relabeling must produce the same schedule identity and source.
        for order in itertools.permutations(range(5)):
            relabeled = "".join(query[order[i] * 5 + order[j]] for i in range(5) for j in range(5))
            other = gm.describe_bitmap_plan(relabeled, kind)
            assert schedule_identity(other, kind) == identity
            assert other["source"] == description["source"]
        offline = gm.precompile_bitmap_plan(query, kind)
        assert Path(offline.module_path).stem == hashlib.sha256(offline.generated_code.encode()).hexdigest()
        verify(offline, graph, kind)
        # A different host graph must find exactly the same compiled artifact.
        complete = csr_graph(nx.complete_graph(12))
        cached = gm.compile_plan(complete, query, kind, pruning_type="none", parallel_type="nested_rt",
                                 scheduler="outgoing", bitmap=True, bitmap_direct=True)
        assert cached.compilation_profile["cache_hit"]
        assert cached.module_path == offline.module_path
        assert cached.generated_code == description["source"]
    assert digest(schedule_identity(gm.describe_bitmap_plan(query, "edge"), "edge")) != \
           digest(schedule_identity(gm.describe_bitmap_plan(query, "vertex"), "vertex"))
    try:
        gm.describe_bitmap_plan(query, "edge_iep")
    except ValueError:
        pass
    else:
        raise AssertionError("IEP was accepted by the bitmap catalog")
    print("PASS: relabeling, stable hashes, cross-graph cache reuse, both semantics and counts")


if __name__ == "__main__":
    main()
