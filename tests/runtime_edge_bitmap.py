"""Ordinary edge-induced bitmap regions versus arrays and array IEP."""
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
parser.add_argument('--benchmark-only', action='store_true')
parser.add_argument('--iep-case-only', action='store_true')
parser.add_argument('--vertices', type=int, default=45, help='Synthetic benchmark graph size')
parser.add_argument('--density', type=float, help='Single benchmark edge probability')
args = parser.parse_args()

def graph(rows):
    ids, offsets = [], [0]
    for row in rows:
        ids.extend(i for i, edge in enumerate(row) if edge)
        offsets.append(len(ids))
    return gm.Graph.from_csr(np.array(offsets, dtype=np.uint64), np.array(ids, dtype=np.uint32))

executions = 0
cases = [(6, []), (7, [(1, 2)]), (8, [(1, 2), (2, 3)]),
         (8, [(1, 2), (1, 3), (2, 3)])]
if args.iep_case_only: cases = cases[-1:]
for n, missing in cases:
    edges = [e for e in itertools.combinations(range(n), 2) if e not in missing]
    query = matrix(n, edges)
    bits = ''.join(str(v) for row in query for v in row)
    if not args.benchmark_only:
        rng = random.Random(900 + n)
        hosts = [matrix(9, itertools.combinations(range(9), 2)),
                 matrix(9, [e for e in itertools.combinations(range(9), 2)
                            if e in edges or rng.random() < .65])]
        for h, rows in enumerate(hosts):
            g = graph(rows)
            expected = count_matches(rows, query, False)
            for parallel in ('openmp', 'nested'):
                for mode, bitmap in [('edge', False), ('edge', True), ('edge_iep', True)]:
                    p = gm.compile_plan(g, bits, mode, scheduler='outgoing', pruning_type='none',
                                        parallel_type=parallel, bitmap=bitmap)
                    active = '// full bitmap region' in p.generated_code
                    assert active == (mode == 'edge' and bitmap), (n, parallel, mode, bitmap)
                    assert 'bitmap-backed IEP' not in p.generated_code
                    if len(missing) == 3 and mode == 'edge_iep':
                        assert '/* Val:' in p.generated_code, 'IEP comparison did not skip suffix loops'
                    for threads in (1, 4):
                        actual = p.run(g, num_threads=threads).number_of_matches
                        assert actual == expected, (n, h, parallel, mode, bitmap, actual, expected)
                        executions += 1
                    print('oracle', n, h, parallel, mode, bitmap, expected, flush=True)
    else:
        for density in ((args.density,) if args.density is not None else (.3, .6)):
            rng = random.Random(2026)
            g = graph(matrix(args.vertices, [e for e in itertools.combinations(range(args.vertices), 2)
                                  if rng.random() < density]))
            variants = [('edge', False), ('edge', True), ('edge_iep', False)]
            plans = [gm.compile_plan(g, bits, mode, scheduler='outgoing', pruning_type='none',
                                     parallel_type='nested_rt', bitmap=bitmap)
                     for mode, bitmap in variants]
            assert '// full bitmap region' in plans[1].generated_code
            iep_active = '/* Val:' in plans[2].generated_code
            if len(missing) == 3: assert iep_active
            samples = [[], [], []]
            counts = set()
            for repeat in range(7):
                for i in ((0, 1, 2) if repeat % 2 == 0 else (2, 1, 0)):
                    result = plans[i].run(g, num_threads=1)
                    counts.add(result.number_of_matches)
                    if repeat: samples[i].append(result.execution_time_seconds)
            assert len(counts) == 1, counts
            times = [statistics.median(s) for s in samples]
            print('timing', n, 'missing', len(missing), density, 'array,bitmap,iep', times,
                  'bitmap_speedup', times[0]/times[1], 'iep_active', iep_active,
                  'matches', counts.pop(), flush=True)
print('Passed edge-induced oracle executions:', executions, flush=True)
