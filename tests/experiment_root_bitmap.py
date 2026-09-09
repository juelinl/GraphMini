"""Train simple per-root bitmap selectors on synthetic data; freeze before Wiki.

Reports a replay of paired root timings, not end-to-end generated-query speedup.
No parallel workers or server access. Artifacts are JSON/JSONL.
"""
import argparse
import gzip
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--wiki', required=True)
parser.add_argument('--phase', choices=['synthetic', 'wiki'], required=True)
parser.add_argument('--root-limit', type=int, default=128)
args = parser.parse_args()
output = Path(args.output)
output.mkdir(parents=True, exist_ok=True)

def write_graph(name, n, edges):
    path = output / (name + '.graph')
    with path.open('w') as f:
        print(n, len(edges), file=f)
        for a, b in sorted(edges): print(a, b, file=f)
    return path

def run(name, n, edges, limit):
    path = write_graph(name, n, edges)
    result = output / (name + '.jsonl')
    started = time.perf_counter()
    with result.open('w') as f:
        subprocess.run([args.binary, str(path), str(limit)], stdout=f, check=True, timeout=150)
    rows = [dict(json.loads(line), graph=name) for line in result.read_text().splitlines()]
    if name.endswith('cliques'):
        assert all(r['matches'] == math.comb(r['prefix'], r['k']-1) and
                   r['triangles'] == math.comb(r['degree'],2) for r in rows)
    if name.endswith('bipartite'):
        assert all(r['matches'] == 0 and r['triangles'] == 0 for r in rows)
    print(name, len(rows), 'root-query pairs', round(time.perf_counter()-started, 2), 'seconds', flush=True)
    return rows

def synthetic(family, seed):
    rng = random.Random(seed)
    n = 198 if family in ('cliques', 'bipartite') else 256 if family in ('sparse', 'hub') else 96
    edges = set()
    for i, j in itertools.combinations(range(n), 2):
        if family == 'cliques': present = i//33 == j//33
        elif family == 'bipartite':
            present = i//66 == j//66 and i%66 < 33 <= j%66 and i%33 != j%33
        elif family == 'hub': present = i == 0 or rng.random() < .015
        else: present = rng.random() < {'sparse': .025, 'medium': .2, 'dense': .6}[family]
        if present: edges.add((i, j))
    labels = list(range(n)); rng.shuffle(labels)
    return n, {tuple(sorted((labels[a], labels[b]))) for a, b in edges}

def candidates(family):
    if family == 'array': return [dict(family=family)]
    if family == 'degree': return [dict(family=family, minimum=t) for t in (8,16,24,32,48,64,96,128,256,100000)]
    if family == 'prefix':
        return [dict(family=family, minimum=t, fraction=f) for t in (8,16,24,32,48,64,96,128,100000)
                for f in (0,.25,.5,.75)]
    if family.endswith('_work'):
        return [dict(family=family, minimum=t, work=w) for t in (8,16,24,32)
                for w in (.5,1,2,4,8,16,32,64,128)]
    return [dict(family=family, minimum=t, density=r) for t in (8,16,24,32,48,64,96,128,100000)
            for r in (0,.1,.2,.4,.6,.8)]

def decision(row, policy):
    family = policy['family']
    if family == 'array': return False, 0
    if family == 'degree': return row['degree'] >= policy['minimum'], 0
    if row['prefix'] < policy['minimum']: return False, 0
    if family == 'prefix': return row['prefix']/max(row['degree'],1) >= policy['fraction'], 0
    density = row['sample_density'] if family.startswith('sample') else 2*row['triangles']/max(row['degree']*(row['degree']-1),1)
    cost = row['sample_ns'] if family.startswith('sample') else row['exact_ns']
    if family.startswith('cached_triangle'): cost = 0  # Counts already available: explicit alternative scenario.
    if family.endswith('_work'):
        # Uniform-density approximation to the number of row uses in a clique
        # suffix. Normalize by degree: build ~d^2, array cost per use ~d.
        uses = sum(math.comb(row['prefix'], depth) * density**(depth*(depth-1)/2)
                   for depth in range(1,row['k']-1))
        return uses/max(row['degree'],1) >= policy['work'], cost
    return density >= policy['density'], cost

def evaluate(rows, policy):
    selected, cost, preprocessing = 0, 0, 0
    for row in rows:
        bitmap, extra = decision(row, policy)
        selected += bitmap
        cost += row['bitmap_ns'] if bitmap else row['array_ns']
        preprocessing += extra
    array = sum(r['array_ns'] for r in rows)
    return dict(policy=policy, roots=len(rows), selected=selected,
                replay_ms=(cost+preprocessing)/1e6, feature_ms=preprocessing/1e6,
                speedup_vs_array=array/(cost+preprocessing) if cost+preprocessing else 1)

families = ('array', 'degree', 'prefix', 'sample', 'triangle', 'cached_triangle',
            'sample_work', 'triangle_work', 'cached_triangle_work')
if args.phase == 'synthetic':
    rng = random.Random(11)
    edges = {e for e in itertools.combinations(range(10),2) if rng.random() < .8}
    checks = run('oracle-small',10,edges,10)
    for row in checks:
        root = row['root']
        prefix = [v for v in range(root) if (v,root) in edges]
        expected = sum(all(tuple(sorted(e)) in edges for e in itertools.combinations(vertices,2))
                       for vertices in itertools.combinations(prefix,row['k']-1))
        assert row['matches'] == expected, (row,expected)
    train, test = [], []
    for split, seed in [('train', 103), ('test', 907)]:
        for family in ('sparse', 'medium', 'dense', 'cliques', 'bipartite', 'hub'):
            n, edges = synthetic(family, seed)
            rows = run(split+'-'+family, n, edges, 48)
            (train if split == 'train' else test).extend(rows)
    policies, summary = {}, {}
    for k in (6,7):
        tr = [r for r in train if r['k'] == k]
        te = [r for r in test if r['k'] == k]
        policies[k] = {family: min(candidates(family), key=lambda p: evaluate(tr,p)['replay_ms']) for family in families}
        summary[k] = dict(test=[evaluate(te,p) for p in policies[k].values()],
                          array_ms=sum(r['array_ns'] for r in te)/1e6,
                          bitmap_ms=sum(r['bitmap_ns'] for r in te)/1e6,
                          oracle_ms=sum(min(r['array_ns'],r['bitmap_ns']) for r in te)/1e6)
    (output/'policies.json').write_text(json.dumps(policies, indent=2))
    (output/'synthetic-summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2), flush=True)
else:
    policies = json.loads((output/'policies.json').read_text())  # Frozen before opening Wiki.
    edges, labels = set(), set()
    with gzip.open(args.wiki, 'rt') as f:
        for line in f:
            if not line.strip() or line.startswith('#'): continue
            a,b = map(int,line.split()[:2]); labels.update((a,b))
            if a != b: edges.add(tuple(sorted((a,b))))
    mapping = {v:i for i,v in enumerate(sorted(labels))}
    edges = {tuple(sorted((mapping[a],mapping[b]))) for a,b in edges}
    rows = run('wiki-'+str(args.root_limit),len(mapping),edges,args.root_limit)
    summary = dict(vertices=len(mapping), edges=len(edges), normalization='undirected, deduplicated, no self-loops, sorted original IDs',
                   sha256=hashlib.sha256(Path(args.wiki).read_bytes()).hexdigest(), queries={})
    for k in (6,7):
        selected = [r for r in rows if r['k'] == k]
        summary['queries'][k] = dict(policies=[evaluate(selected,p) for p in policies[str(k)].values()],
            array_ms=sum(r['array_ns'] for r in selected)/1e6, bitmap_ms=sum(r['bitmap_ns'] for r in selected)/1e6,
            oracle_ms=sum(min(r['array_ns'],r['bitmap_ns']) for r in selected)/1e6)
    (output/('wiki-'+str(args.root_limit)+'-summary.json')).write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary,indent=2),flush=True)
