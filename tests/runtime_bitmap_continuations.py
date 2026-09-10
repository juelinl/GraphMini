"""Native checks for a single BitGraph decision and dependency-scoped conversions.

Run serially with other runtime-compilation tests, with the build's lib on
PYTHONPATH. Covers root/late entry, direct projection, and budget fallback.
"""
import argparse
import ctypes
import itertools
import math
import random
import re

import graphmini as gm
import numpy as np

from induced_subset_oracle import count_induced_subsets
from matching_oracle import count_matches, matrix
from runtime_bitmap_hoist import PATTERNS
from runtime_test_support import restore_generated_plan_at_exit


def graph(rows):
    offsets, indices = [0], []
    for row in rows:
        indices.extend(sorted(row))
        offsets.append(len(indices))
    return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64),
                             np.array(indices, dtype=np.uint32))


def host(data):
    return graph([{j for j, edge in enumerate(row) if edge} for row in data])


def clique(n):
    return "".join("1" if i != j else "0" for i in range(n) for j in range(n))


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parallel", default="nested_rt",
                        choices=["openmp", "tbb_top", "nested", "nested_rt"])
    args = parser.parse_args()
    calibration = host(matrix(9, itertools.combinations(range(9), 2)))
    cases = [("K5", clique(5)), ("anchor1", PATTERNS[145]),
             ("late-entry", PATTERNS[190]), ("projection", PATTERNS[204]),
             ("K7", clique(7)), ("K8", clique(8))]
    checks = 0
    for name, bits in cases:
        n = math.isqrt(len(bits))
        query = [[int(bits[i*n+j]) for j in range(n)] for i in range(n)]
        for semantics in ("vertex", "edge"):
            for direct in ((False, True) if name == "projection" else (True,)):
                plan = gm.compile_plan(
                    calibration, bits, semantics, scheduler="outgoing",
                    pruning_type="none", parallel_type=args.parallel,
                    bitmap=True, bitmap_direct=direct, bitmap_diagnostics=True)
                code = re.sub(r"\s+", "", plan.generated_code)
                assert code.count("if(bitmap_rows)") == 1, (name, semantics)
                assert "if(!bitmap_rows)" not in code and "std::optional<Bitmap>" not in code
                assert "bitmap-enabledcontinuation" in code and "array-onlycontinuation" in code
                counter = ctypes.CDLL(plan.module_path).graphmini_bitmap_counter
                counter.argtypes, counter.restype = [ctypes.c_uint], ctypes.c_uint64
                rng = random.Random(713)
                for sample in range(4):
                    # Plant the query, vary extra vertices/edges, then relabel.
                    data = matrix(n + 1, [e for e in itertools.combinations(range(n + 1), 2)
                                         if rng.random() < sample / 3])
                    for i in range(n):
                        for j in range(n):
                            data[i][j] = query[i][j]
                    order = list(range(n + 1))
                    rng.shuffle(order)
                    data = [[data[i][j] for j in order] for i in order]
                    expected = (count_induced_subsets(data, query) if semantics == "vertex" or name.startswith("K")
                                else count_matches(data, query, False))
                    for threads in (1, 4):
                        actual = plan.run(host(data), num_threads=threads).number_of_matches
                        assert actual == expected, (name, semantics, direct, sample, threads, actual, expected)
                        checks += 1
                if name in ("K5", "projection"):
                    # A unique matching core plus pendant leaves exercises tiny,
                    # word-boundary, and rejected universes in the same kernel.
                    for degree in (64, 128, 512, 20000):
                        rows = [set() for _ in range(degree + 1)]
                        core = list(range(degree - n + 1, degree + 1))
                        for i in range(n):
                            for j in range(n):
                                if query[i][j]:
                                    rows[core[i]].add(core[j])
                        for leaf in range(degree - n + 1):
                            rows[leaf].add(degree)
                            rows[degree].add(leaf)
                        for threads in (1, 4):
                            actual = plan.run(graph(rows), num_threads=threads).number_of_matches
                            assert actual == 1, (name, semantics, degree, actual)
                            if degree == 20000 and semantics == "vertex":
                                assert counter(5) > 0 and counter(6) == 1, "Missing positive array fallback"
                            checks += 1
                print("PASS", name, semantics, "direct", direct, args.parallel, flush=True)
    print(f"Passed {checks} single-decision native checks ({args.parallel})", flush=True)


if __name__ == "__main__":
    main()
