"""Controlled hoist-only versus shared-projection experiment; no policy changes."""
import argparse
import itertools
import json
from pathlib import Path
import random
import statistics

import graphmini as gm
from benchmark_bitmap_server import make_graph, hosts
from induced_subset_oracle import count_induced_subsets
from matching_oracle import matrix
from runtime_bitmap_hoist import PATTERNS
from runtime_test_support import restore_generated_plan_at_exit


def graph(data):
    return make_graph([{j for j, edge in enumerate(row) if edge} for row in data])


def main():
    restore_generated_plan_at_exit()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--parallel', default='nested_rt', choices=['nested_rt', 'nested', 'openmp'])
    parser.add_argument('--wiki', action='store_true')
    parser.add_argument('--threads', default='1,4')
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    threads_list = list(map(int, args.threads.split(',')))
    rng = random.Random(20260909)
    query = [[int(PATTERNS[204][i*6+j]) for j in range(6)] for i in range(6)]
    dense = matrix(64, [e for e in itertools.combinations(range(64), 2) if rng.random() < .55])
    calibration = graph(dense)
    options = dict(scheduler='outgoing', pruning_type='none', parallel_type=args.parallel, bitmap=True)
    plans = [gm.compile_plan(calibration, PATTERNS[204], 'vertex', **options, bitmap_direct=direct)
             for direct in (False, True)]
    assert 'shared bounded neighborhood projection' not in plans[0].generated_code
    assert 'shared bounded neighborhood projection' in plans[1].generated_code
    assert '->bind_input(' not in plans[1].generated_code
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix('.cpp').write_text(plans[1].generated_code)
    checks = 0
    # Random graphs, planted instances, no-match hosts and relabelled hosts.
    for sample in range(24):
        data = matrix(10, [e for e in itertools.combinations(range(10), 2)
                           if rng.random() < (sample % 10) / 10])
        if sample % 2:
            for i in range(6):
                for j in range(6):
                    data[i][j] = query[i][j]
        order = list(range(10))
        rng.shuffle(order)
        data = [[data[i][j] for j in order] for i in order]
        expected = count_induced_subsets(data, query)
        host = graph(data)
        for plan in plans:
            for threads in threads_list:
                assert plan.run(host, num_threads=threads).number_of_matches == expected
                checks += 1
    # Variable word counts, empty/tail masks, and positive budget fallback.
    for degree in (63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 20000):
        adjacency = [set() for _ in range(degree + 1)]
        core = list(range(degree - 5, degree + 1))
        for i in range(6):
            for j in range(6):
                if query[i][j]: adjacency[core[i]].add(core[j])
        for leaf in range(degree - 5):
            adjacency[leaf].add(degree)
            adjacency[degree].add(leaf)
        host = make_graph(adjacency)
        for plan in plans:
            for threads in threads_list:
                assert plan.run(host, num_threads=threads).number_of_matches == 1
                checks += 1
    print(f'Passed {checks} oracle/planted checks', flush=True)
    if args.verify_only:
        args.output.write_text(json.dumps(dict(checks=checks, parallel=args.parallel)) + '\n')
        return
    measured = [('synthetic-64-p55', calibration)]
    for n, density in [(128, .08), (128, .3), (64, .8)]:
        data = matrix(n, [e for e in itertools.combinations(range(n), 2) if rng.random() < density])
        measured.append((f'synthetic-{n}-p{density}', graph(data)))
    if args.wiki:
        _, wiki, _ = next(hosts(Path(__file__).resolve().parents[1] / '.verification/unity/data', ['wiki-Vote']))
        measured.append(('wiki-Vote', wiki))
    records = []
    for name, host in measured:
        for threads in threads_list:
            samples = [[], []]
            counts = set()
            for repeat in range(args.trials + 1):
                for i in ((0, 1) if repeat % 2 == 0 else (1, 0)):
                    result = plans[i].run(host, num_threads=threads)
                    counts.add(result.number_of_matches)
                    if repeat: samples[i].append(result.execution_time_seconds)
            assert len(counts) == 1
            medians = [statistics.median(s) for s in samples]
            record = dict(graph=name, threads=threads, parallel=args.parallel, count=counts.pop(),
                          compile_metadata_host='synthetic-64-p55',
                          hoist_seconds=medians[0], direct_seconds=medians[1],
                          speedup=medians[0]/medians[1], samples=samples)
            records.append(record)
            print(json.dumps(record), flush=True)
            args.output.write_text(json.dumps(dict(checks=checks, records=records), indent=2) + '\n')


if __name__ == '__main__':
    main()
