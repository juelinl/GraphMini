"""Time frozen synthetic-trained policies in the single-thread clique harness."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True)
p.add_argument('--directory', required=True)
args = p.parse_args()
root = Path(args.directory)
policies = json.loads((root/'policies.json').read_text())
specs = [('array','array'), ('bitmap','bitmap')]
for name in ('degree','prefix','sample','triangle'):
    a, b = policies['6'][name], policies['7'][name]
    assert a == b, 'Runner needs separate query-specific policy invocations'
    value = a.get('fraction',a.get('density',0))
    specs.append((name,f'{name}:{a["minimum"]}:{value}'))
results = []
for graph in ('test-dense','test-cliques','test-bipartite','wiki-7115'):
    expected = {}
    for repeat in range(2 if graph == 'wiki-7115' else 1):
        order = specs if repeat == 0 else list(reversed(specs))
        for name, spec in order:
            output = subprocess.check_output([args.binary,str(root/(graph+'.graph')),'100000',spec],
                                             text=True,timeout=150)
            rows = [json.loads(line) for line in output.splitlines()]
            for row in rows:
                k = row['k']
                if k in expected: assert expected[k] == row['matches'], (graph,spec,row)
                expected[k] = row['matches']
                row.update(graph=graph,repeat=repeat,spec=spec)
                results.append(row)
                print(json.dumps(row),flush=True)
            (root/'timed-policies.json').write_text(json.dumps(results,indent=2))
