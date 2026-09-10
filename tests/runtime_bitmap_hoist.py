"""Bounded late-entry BitGraph regression and before/after timing experiment.

Run serially with other runtime compilation tests. --baseline records the old
placement without enforcing the new build-count invariant. Timings use separate
non-instrumented plans; construction counters come from a diagnostic run.
"""
import argparse
import ctypes
import itertools
import json
from pathlib import Path
import random
import statistics

import graphmini as gm
from benchmark_bitmap_server import make_graph, hosts
from induced_subset_oracle import count_induced_subsets
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

PATTERNS = {
    145: "011011101000110101001010100100101000",  # anchor 1, entry 2
    190: "011001101110110001010011010101101110",  # anchor 0, entry 2
    204: "011011101101110110011011101101110110",  # anchor 0, entry 2
    208: "011111101111110111111011111101111110",  # root-entry control
}


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--parallel", default="nested_rt", choices=["openmp", "nested_rt", "nested"])
    parser.add_argument("--patterns", default="145,190,204,208")
    parser.add_argument("--wiki", action="store_true")
    args = parser.parse_args()
    rng = random.Random(20260909)
    dense = matrix(64, [e for e in itertools.combinations(range(64), 2) if rng.random() < .55])
    g = make_graph([{j for j, b in enumerate(row) if b} for row in dense])
    benchmark_graphs = [("synthetic-64-p55", g, 64)]
    if args.wiki:
        _, wiki, meta = next(hosts(Path(__file__).resolve().parents[1] / ".verification/unity/data", ["wiki-Vote"]))
        benchmark_graphs.append(("wiki-Vote", wiki, meta["vertices"]))
    records, checks = [], 0
    for atlas in map(int, args.patterns.split(",")):
        bits = PATTERNS[atlas]
        query = [[int(bits[i * 6 + j]) for j in range(6)] for i in range(6)]
        plans = [gm.compile_plan(g, bits, "vertex", scheduler="outgoing", pruning_type="none",
                                 parallel_type=args.parallel, bitmap=bitmap) for bitmap in (False, True)]
        assert "// full bitmap region" in plans[1].generated_code
        if not args.baseline:
            assert "BitGraph::build" in plans[1].generated_code
            assert "std::optional<Bitmap> bitmap_s" in plans[1].generated_code
        diagnostic = gm.compile_plan(g, bits, "vertex", scheduler="outgoing", pruning_type="none",
                                     parallel_type=args.parallel, bitmap=True, bitmap_diagnostics=True)
        counter = ctypes.CDLL(diagnostic.module_path).graphmini_bitmap_counter
        counter.argtypes, counter.restype = [ctypes.c_uint], ctypes.c_uint64
        # Count matching subsets once, not embeddings divided by a symmetry factor.
        for sample in range(8):
            small = matrix(10, [e for e in itertools.combinations(range(10), 2)
                               if rng.random() < (sample + 1) / 10])
            for i in range(6):
                for j in range(6):
                    small[i][j] = query[i][j]
            permutation = list(range(10))
            rng.shuffle(permutation)
            small = [[small[i][j] for j in permutation] for i in permutation]
            expected = count_induced_subsets(small, query)
            host = make_graph([{j for j, b in enumerate(row) if b} for row in small])
            for plan in plans + [diagnostic]:
                for threads in (1, 4):
                    assert plan.run(host, num_threads=threads).number_of_matches == expected
                    checks += 1
        if atlas == 204:
            # A planted octahedron and degree-one leaves. Leaves cannot appear
            # in a match (every query vertex has degree four). The highest-ID
            # anchor has an over-budget universe, so the sole match must survive
            # rejection at the hoisted build and use the late-entry fallback.
            degree = 20000
            adjacency = [set() for _ in range(degree + 1)]
            core = list(range(degree - 5, degree + 1))
            for i in range(6):
                for j in range(6):
                    if query[i][j]:
                        adjacency[core[i]].add(core[j])
            for leaf in range(degree - 5):
                adjacency[leaf].add(degree)
                adjacency[degree].add(leaf)
            host = make_graph(adjacency)
            for plan in plans + [diagnostic]:
                for threads in (1, 4):
                    assert plan.run(host, num_threads=threads).number_of_matches == 1
                    checks += 1
                    if plan is diagnostic:
                        assert counter(5) > 0 and counter(6) == 1, "Late-entry fallback missed the planted match"
        for name, host, vertices in benchmark_graphs:
            for threads in (1, 4):
                samples = [[], []]
                counts = set()
                for repeat in range(6):
                    for backend in ((0, 1) if repeat % 2 == 0 else (1, 0)):
                        result = plans[backend].run(host, num_threads=threads)
                        counts.add(result.number_of_matches)
                        if repeat:
                            samples[backend].append(result.execution_time_seconds)
                assert len(counts) == 1
                assert diagnostic.run(host, num_threads=threads).number_of_matches in counts
                builds, rows, conversions = [counter(i) for i in range(3)]
                if not args.baseline and atlas != 145:
                    assert builds <= vertices, (atlas, builds, vertices)
                record = dict(atlas_id=atlas, graph=name, threads=threads,
                              parallel=args.parallel, baseline=args.baseline,
                              count=counts.pop(), builds=builds, rows=rows, conversions=conversions,
                              array_seconds=statistics.median(samples[0]),
                              bitmap_seconds=statistics.median(samples[1]), samples=samples)
                records.append(record)
                print(json.dumps(record), flush=True)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(dict(oracle_checks=checks, records=records), indent=2) + "\n")
    print(f"Passed {checks} independent subset-oracle checks", flush=True)


if __name__ == "__main__":
    main()
