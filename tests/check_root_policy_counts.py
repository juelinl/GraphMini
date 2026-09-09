"""Cross-check normalized Wiki-Vote clique counts against generated kernels."""
import argparse
import numpy as np
import graphmini as gm
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
parser = argparse.ArgumentParser()
parser.add_argument('graph', help='Normalized undirected Wiki-Vote text graph')
args = parser.parse_args()
with open(args.graph) as source:
    n, m = map(int, next(source).split())
    rows = [[] for _ in range(n)]
    for line in source:
        u, v = map(int, line.split())
        rows[u].append(v)
        rows[v].append(u)
offsets, ids = [0], []
for row in rows:
    ids.extend(sorted(set(row)))
    offsets.append(len(ids))
graph = gm.Graph.from_csr(np.array(offsets, dtype=np.uint64),
                          np.array(ids, dtype=np.uint32))
for k, expected in ((6, 6931312), (7, 8113409)):
    pattern = ''.join(str(int(i != j)) for i in range(k) for j in range(k))
    for bitmap in (False, True):
        plan = gm.compile_plan(graph, pattern, 'edge', scheduler='outgoing',
                               pruning_type='none', parallel_type='nested_rt',
                               bitmap=bitmap)
        actual = plan.run(graph, num_threads=1).number_of_matches
        assert actual == expected, (k, bitmap, actual, expected)
        print('verified', k, 'bitmap', bitmap, 'count', actual, flush=True)
