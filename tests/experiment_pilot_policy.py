"""Actual single-thread pilot timings; synthetic training before frozen Wiki test."""
import argparse
import json
import math
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True)
p.add_argument('--directory', required=True)
p.add_argument('--family', choices=['pilot', 'probe'], default='pilot')
args = p.parse_args()
root = Path(args.directory)
records, counts = [], {}

def run(graph, spec, repeat=0):
    output = subprocess.check_output(
        [args.binary, str(root/(graph+'.graph')), '100000', spec], text=True, timeout=150)
    rows = [json.loads(line) for line in output.splitlines()]
    for row in rows:
        key = graph, row['k']
        if key in counts:
            assert row['matches'] == counts[key], (graph, spec, row)
        counts[key] = row['matches']
        row.update(graph=graph, spec=spec, repeat=repeat)
        records.append(row)
        print(json.dumps(row), flush=True)
    (root/(args.family+'-timings.json')).write_text(json.dumps(records, indent=2))
    return rows

families = ('sparse', 'medium', 'dense', 'cliques', 'bipartite', 'hub')
pilots = [f'{args.family}:{n}:{threshold}' for n in (2,4) for threshold in (1,4,16)]
baseline = ['array', 'bitmap', 'degree:32:0']
# Validate every candidate against array counts on the independent tiny fixture.
for spec in baseline + pilots:
    run('oracle-small', spec)
# All actual training timings include pilot traversal and the decision cost.
for family in families:
    for spec in baseline + pilots:
        run('train-'+family, spec)
train = {(r['graph'],r['k'],r['spec']): r['total_ns']
         for r in records if r['graph'].startswith('train-')}
def score(spec):
    # Equal weight per graph/query, avoiding domination by dense-query runtime.
    return sum(math.log(train['train-'+f,k,spec]/train['train-'+f,k,'array'])
               for f in families for k in (6,7)) / (len(families)*2)
winner = min(pilots, key=score)
frozen = dict(policy=winner, objective='mean log runtime ratio versus arrays across training graph/query pairs',
              scores={s: math.exp(score(s)) for s in baseline+pilots})
(root/(args.family+'-policy.json')).write_text(json.dumps(frozen, indent=2))
print('FROZEN', json.dumps(frozen), flush=True)
for family in families:
    for spec in baseline+[winner]:
        run('test-'+family,spec)
# No threshold changes after this point. Two reversed-order whole-query runs.
for repeat in range(2):
    specs = baseline+[winner]
    for spec in specs if repeat == 0 else reversed(specs):
        run('wiki-7115',spec,repeat)
