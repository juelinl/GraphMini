"""Local sequential CPU/Metal benchmark. Synthetic fixtures precede full Wiki.

Uses fixtures emitted by experiment_root_bitmap.py. CPU medians include region
construction; Metal medians additionally include graph validation and dispatch.
No GPU validation instrumentation during timing. No servers or remote jobs.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--metal', required=True)
p.add_argument('--cpu', required=True)
p.add_argument('--fixtures', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
all_rows, summaries = [], []
graphs = ['test-sparse','test-medium','test-dense','test-cliques','test-bipartite','test-hub','wiki-7115']
for name in graphs:
    graph = args.fixtures/(name+'.graph')
    records = []
    expected = {}
    for repeat in range(2):
        modes = ['array','bitmap','metal']
        if repeat: modes.reverse()
        for mode in modes:
            if mode == 'metal':
                for k in (6,7):
                    output = subprocess.check_output([args.metal,str(graph),str(k),'3'], text=True,timeout=180)
                    rows = [dict(json.loads(line),mode=mode,repeat=repeat,graph=name) for line in output.splitlines()]
                    for row in rows:
                        if k in expected: assert row['matches'] == expected[k], (name,row,expected)
                        expected[k] = row['matches']
                    records.extend(rows)
            else:
                output = subprocess.check_output([args.cpu,str(graph),'100000',mode],text=True,timeout=180)
                rows = [dict(json.loads(line),mode=mode,repeat=repeat,graph=name) for line in output.splitlines()]
                for row in rows:
                    k = row['k']
                    if k in expected: assert row['matches'] == expected[k], (name,row,expected)
                    expected[k] = row['matches']
                records.extend(rows)
    if name == 'wiki-7115': assert expected == {6:6931312,7:8113409}, expected
    all_rows.extend(records)
    for k in (6,7):
        r = [x for x in records if x['k'] == k]
        metal = [x for x in r if x['mode'] == 'metal' and x['trial'] > 0]
        summary = dict(graph=name,k=k,matches=expected[k],
            array_ms=[x['total_ns']/1e6 for x in r if x['mode']=='array'],
            bitmap_ms=[x['total_ns']/1e6 for x in r if x['mode']=='bitmap'],
            metal_ms=statistics.median(x['total_ms'] for x in metal),
            gpu_ms=statistics.median(x['gpu_ms'] for x in metal),
            construction_ms=statistics.median(x['construction_ms'] for x in metal),
            validation_ms=statistics.median(x['validation_ms'] for x in metal),
            batches=metal[0]['batches'],tasks=metal[0]['tasks'],
            peak_buffer_bytes=metal[0]['peak_buffer_bytes'])
        summaries.append(summary)
        print(json.dumps(summary),flush=True)
    (args.output/'timings.json').write_text(json.dumps(all_rows,indent=2))
    (args.output/'summary.json').write_text(json.dumps(summaries,indent=2))
