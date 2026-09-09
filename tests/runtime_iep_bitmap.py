"""Verify the array-only IEP policy, including callers passing bitmap=True."""
import argparse
import itertools
import random
import statistics
import numpy as np
import graphmini as gm
from matching_oracle import matrix, count_matches
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
parser = argparse.ArgumentParser()
parser.add_argument('--benchmark', action='store_true')
parser.add_argument('--wide', action='store_true', help='Use four independent suffix vertices')
parser.add_argument('--benchmark-only', action='store_true', help='Skip exhaustive oracle checks')
args = parser.parse_args()

def graph(rows):
    ids, offsets = [], [0]
    for row in rows:
        ids.extend(i for i, edge in enumerate(row) if edge)
        offsets.append(len(ids))
    return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64),
                             np.array(ids, dtype=np.uint32))

selected = 0
for n in (6, 7, 8):
    leaves = 4 if args.wide and n >= 7 else 3
    core = n - leaves
    edges = list(itertools.combinations(range(core), 2))
    edges += [(a, core + leaf) for leaf in range(leaves) for a in range(core)
              if a == 0 or a != leaf % core + 1]
    query = matrix(n, edges)
    bits = ''.join(str(x) for row in query for x in row)
    rng = random.Random(100 + n)
    hosts = [matrix(9, itertools.combinations(range(9), 2)),
             matrix(9, [e for e in itertools.combinations(range(9), 2)
                        if rng.random() < .75 or e in edges])]
    if args.benchmark_only:
        hosts = []
    for h, rows in enumerate(hosts):
        g = graph(rows)
        expected = count_matches(rows, query, False)
        for parallel in ('openmp', 'nested_rt'):
            for bitmap in (False, True):
                p = gm.compile_plan(g, bits, 'edge_iep', scheduler='outgoing',
                                    pruning_type='none', parallel_type=parallel,
                                    bitmap=bitmap)
                active = 'bitmap-backed IEP' in p.generated_code
                assert not active, 'IEP must remain array-based even with bitmap=True'
                selected += active
                for threads in (1, 4):
                    actual = p.run(g, num_threads=threads).number_of_matches
                    assert actual == expected, (n, h, parallel, bitmap, actual, expected)
                print('oracle', n, h, parallel, bitmap, 'active', active, 'count', expected, flush=True)
    if args.benchmark or args.benchmark_only:
        for density in (.15, .6):
            rng = random.Random(2026)
            g = graph(matrix(45, [e for e in itertools.combinations(range(45), 2)
                                  if rng.random() < density]))
            plans = [gm.compile_plan(g, bits, 'edge_iep', scheduler='outgoing',
                                    pruning_type='none', parallel_type='nested_rt',
                                    bitmap=b) for b in (False, True)]
            samples = [[], []]
            counts = set()
            for repeat in range(7):
                for b in ((0, 1) if repeat % 2 == 0 else (1, 0)):
                    result = plans[b].run(g, num_threads=1)
                    counts.add(result.number_of_matches)
                    if repeat:
                        samples[b].append(result.execution_time_seconds)
            assert len(counts) == 1, counts
            times = [statistics.median(s) for s in samples]
            print('timing', n, density, times, 'speedup', times[0]/times[1], flush=True)
if not args.benchmark_only:
    assert selected == 0, 'IEP unexpectedly selected bitmap execution'
    print('All 48 oracle executions passed; bitmap plans selected:', selected, flush=True)
