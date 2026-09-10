"""Check IEP-first selection for the exact 16-pattern paper comparison cohort."""
import sys
from pathlib import Path
import graphmini as gm

PATTERNS = [
    "000111000111000011110011111101111110",
    "000111000111000111111001111001111110",
    "000111000111000111111011111101111110",
    "001011000111100011010011111101111110",
    "001111000111100011110001111001111110",
    "001111000111100011110011111101111110",
    "001111000011100111101001111001111110",
    "001111000001100111101011101101111110",
    "001111000011100111101011111101111110",
    "001111000111100111111011111101111110",
    "001111001111110011110001111000111100",
    "001111001111110011110000111001111010",
    "001111001110110011110001111001101110",
    "001111001111110011110001111001111110",
    "001111001111110011110011111100111100",
    "001111001111110011110011111101111110",
]


def main():
    for i, query in enumerate(PATTERNS):
        edge = gm.describe_offline_plan(query, "edge")
        forced = gm.describe_bitmap_plan(query, "edge")
        expected = "iep" if i < 3 else "array" if i == 12 else "bitmap"
        assert edge["strategy"] == expected, (i, edge["strategy"])
        assert edge["iep_width"] == (2 if i < 3 else 0)
        assert edge["execution_query_type"] == ("edge_iep" if i < 3 else "edge")
        assert gm.describe_offline_plan(query, "vertex")["strategy"] != "iep"
        assert forced["strategy"] != "iep"
        if i < 3:
            assert not edge["bitmap_selected"]
            assert edge["source"] and edge["source"] != forced["source"]
        else:
            assert edge["source"] == forced["source"]
    if "--compile" in sys.argv:
        import networkx as nx
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from precompile_bitmap_plans import csr_graph, verify
        from runtime_test_support import restore_generated_plan_at_exit
        restore_generated_plan_at_exit()
        for query in PATTERNS[:3]:
            graph = nx.Graph()
            graph.add_nodes_from(range(6))
            graph.add_edges_from((i, j) for i in range(6) for j in range(i + 1, 6) if query[i * 6 + j] == "1")
            offline = gm.precompile_offline_plan(query, "edge")
            assert offline.generated_code == gm.describe_offline_plan(query, "edge")["source"]
            verify(offline, graph, "edge")
            # Explicit IEP mode shares this cache entry, even on another graph.
            cached = gm.compile_plan(csr_graph(nx.complete_graph(9)), query, "edge_iep",
                                     pruning_type="none", parallel_type="nested_rt", scheduler="outgoing")
            assert cached.compilation_profile["cache_hit"]
            assert cached.module_path == offline.module_path
    print("PASS: IEP for 0/1/2; bitmap for remaining eligible edge patterns; vertex unchanged")


if __name__ == "__main__":
    main()
