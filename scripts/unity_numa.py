"""Execute only when the Slurm allocation contains a complete NUMA domain."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def cpu_list(text):
    cpus = set()
    for item in text.strip().split(','):
        bounds = list(map(int, item.split('-')))
        cpus.update(range(bounds[0], bounds[-1] + 1))
    return cpus


def choose_domain(allowed, online, nodes, threads):
    candidates = [(node, sorted(cpus & online)) for node, cpus in nodes
                  if len(cpus & online) >= threads and (cpus & online) <= allowed]
    if not candidates:
        raise RuntimeError(f'Allocation {sorted(allowed)} does not own an entire NUMA domain; refusing benchmark')
    return sorted(candidates, key=lambda pair: (-len(pair[1]), pair[0]))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', required=True)
    parser.add_argument('--threads', type=int, default=12)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not os.environ.get('SLURM_JOB_ID'):
        raise RuntimeError('Must run inside a Slurm compute allocation')
    allowed = set(os.sched_getaffinity(0))
    online = cpu_list(Path('/sys/devices/system/cpu/online').read_text())
    nodes = []
    for node in Path('/sys/devices/system/node').glob('node[0-9]*'):
        nodes.append((int(node.name[4:]), cpu_list((node / 'cpulist').read_text())))
    node, cpus = choose_domain(allowed, online, nodes, args.threads)
    metadata = dict(hostname=os.uname().nodename, numa_node=node, numa_cpus=cpus,
                    allocated_affinity=sorted(allowed), matching_threads=args.threads,
                    slurm_job=os.environ['SLURM_JOB_ID'],
                    topology=subprocess.check_output(['lscpu', '--json'], text=True),
                    slurm_allocation=subprocess.check_output(['scontrol', 'show', 'job', os.environ['SLURM_JOB_ID']], text=True))
    Path(args.metadata).write_text(json.dumps(metadata, indent=2) + '\n')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        raise RuntimeError('Missing command')
    print(f'Owned NUMA domain {node}: {cpus}; {args.threads} matching threads', flush=True)
    os.execvp('numactl', ['numactl', '--physcpubind=' + ','.join(map(str, cpus)),
                         f'--membind={node}', *command])


if __name__ == '__main__':
    main()
