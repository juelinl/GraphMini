"""Pack two independent NUMA workers into a whole-node Slurm allocation."""
import json
import os
from pathlib import Path
import subprocess
import sys

from unity_numa import cpu_list, physical_cpus


def partition_patterns(patterns, workers):
    if not patterns or workers < 1 or len(patterns) < workers:
        raise ValueError('Every NUMA worker must have a pattern')
    return [patterns[i::workers] for i in range(workers)]


def main():
    root = Path('/home/juelinliu_umass_edu/GraphMini')
    job = os.environ['SLURM_JOB_ID']
    allowed = set(os.sched_getaffinity(0))
    online = cpu_list(Path('/sys/devices/system/cpu/online').read_text())
    if not online <= allowed:
        raise RuntimeError('Whole-node CPU allocation required before launching NUMA workers')
    domains = []
    for path in sorted(Path('/sys/devices/system/node').glob('node[0-9]*')):
        cpus = cpu_list((path / 'cpulist').read_text()) & online
        topology = {}
        for cpu in cpus:
            base = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
            topology[cpu] = tuple(int((base / field).read_text()) for field in ('physical_package_id', 'core_id'))
        if cpus:
            domains.append((int(path.name[4:]), physical_cpus(cpus, topology)))
    if len(domains) != 2 or any(len(cpus) != 32 for _, cpus in domains):
        raise RuntimeError(f'Expected two 32-core NUMA domains on Xeon 8352Y, got {domains}')
    corpus = json.loads((root / 'atlas6-corpus.json').read_text())['patterns']
    if 'SLURM_ARRAY_TASK_ID' in os.environ:
        index = int(os.environ['SLURM_ARRAY_TASK_ID'])
        patterns = [p['atlas_id'] for p in corpus[index * 2:index * 2 + 2]]
    else:
        patterns = [117, 158, 207, 208]
    groups = partition_patterns(patterns, len(domains))
    folder = root / 'runs' / f'node-{job}'
    folder.mkdir(exist_ok=False)
    (folder / 'allocation.json').write_text(json.dumps(dict(job=job, host=os.uname().nodename,
        domains=domains, groups=groups, affinity=sorted(allowed),
        slurm=subprocess.check_output(['scontrol', 'show', 'job', job], text=True)), indent=2))
    children = []
    try:
        for (node, cpus), group in zip(domains, groups):
            env = dict(os.environ, GRAPHMINI_NUMA_NODE=str(node),
                       GRAPHMINI_ATLAS_IDS=','.join(map(str, group)))
            log = (folder / f'numa{node}.log').open('w')
            command = [sys.executable, str(root / 'source/scripts/unity_numa.py'),
                '--node', str(node), '--metadata', str(folder / f'numa{node}.json'), '--',
                'bash', '-l', str(root / 'source/scripts/unity_benchmark.sh')]
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            children.append((process, log))
            print(f'NUMA {node}: {len(cpus)} physical cores, atlas {group}, pid {process.pid}', flush=True)
        codes = [process.wait() for process, _ in children]
        if any(codes):
            raise RuntimeError(f'NUMA worker exit codes: {codes}; inspect {folder}')
    finally:
        for process, log in children:
            if process.poll() is None:
                process.terminate()
                process.wait()
            log.close()
    print('NODE_BENCHMARK_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
