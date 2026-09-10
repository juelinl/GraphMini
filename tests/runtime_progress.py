"""Check process-surviving progress against symmetry-normalized counts."""
import itertools
import os
from pathlib import Path
import tempfile

import graphmini as gm
from benchmark_atlas_runtime import read_progress
from benchmark_bitmap_server import make_graph
from induced_subset_oracle import count_induced_subsets
from matching_oracle import matrix
from runtime_test_support import restore_generated_plan_at_exit

restore_generated_plan_at_exit()
with tempfile.TemporaryDirectory() as directory:
    for missing in ([], [(0, 1)], [(0, 1), (2, 3)]):
        query = matrix(6, [e for e in itertools.combinations(range(6), 2) if e not in missing])
        host = matrix(9, [e for e in itertools.combinations(range(9), 2) if e not in missing])
        graph = make_graph([{j for j, bit in enumerate(row) if bit} for row in host])
        expected = count_induced_subsets(host, query)
        for bitmap in (False, True):
            plan = gm.compile_plan(graph, ''.join(str(v) for row in query for v in row),
                                   'vertex', scheduler='outgoing', pruning_type='none',
                                   parallel_type='nested', bitmap=bitmap)
            for threads in (1, 4):
                path = Path(directory) / f'{len(missing)}-{bitmap}-{threads}.bin'
                os.environ['GRAPHMINI_PROGRESS_FILE'] = str(path)
                try:
                    result = plan.run(graph, num_threads=threads)
                finally:
                    os.environ.pop('GRAPHMINI_PROGRESS_FILE')
                snapshot = read_progress(path)
                assert snapshot['completed_root_vertex_ids'] == list(range(9)), snapshot
                assert snapshot['accumulated_matches'] == result.number_of_matches == expected
                # Disabled runs must not retain a pointer to the previous mapping.
                assert plan.run(graph, num_threads=threads).number_of_matches == expected
                # A failed progress setup must leave the reusable plan intact.
                os.environ['GRAPHMINI_PROGRESS_FILE'] = str(path)
                try:
                    try:
                        plan.run(graph, num_threads=threads)
                    except RuntimeError as error:
                        assert 'Cannot create benchmark progress file' in str(error)
                    else:
                        raise AssertionError('Existing progress file was unexpectedly accepted')
                finally:
                    os.environ.pop('GRAPHMINI_PROGRESS_FILE')
                assert plan.run(graph, num_threads=threads).number_of_matches == expected
print('Progress counts and root coverage verified for array/bitmap, nested TBB, 1/4 threads')
