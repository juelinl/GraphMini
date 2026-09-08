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


def choose_domain(allowed, online, nodes, threads, allow_shared=False):
    candidates = [(node, sorted(cpus & online & allowed)) for node, cpus in nodes
                  if len(cpus & online & allowed) >= threads
                  and (allow_shared or (cpus & online) <= allowed)]
    if not candidates:
        raise RuntimeError(f'Allocation {sorted(allowed)} does not own an entire NUMA domain; refusing benchmark')
    return sorted(candidates, key=lambda pair: (-len(pair[1]), pair[0]))[0]


def physical_cpus(cpus, topology):
    """One logical CPU per physical (socket, core), keeping SMT out of timings."""
    selected = {}
    for cpu in sorted(cpus):
        selected.setdefault(topology[cpu], cpu)
    return sorted(selected.values())


def benchmark_command(command, threads):
    return [arg.replace('{numa_threads}', str(threads)) for arg in command]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', required=True)
    parser.add_argument('--threads', type=int, help='Default: every physical core in the selected NUMA domain')
    parser.add_argument('--node', type=int, default=int(os.environ['GRAPHMINI_NUMA_NODE']) if 'GRAPHMINI_NUMA_NODE' in os.environ else None)
    parser.add_argument('--allow-shared', action='store_true', default=os.environ.get('GRAPHMINI_SHARED_NUMA') == '1',
                        help='Use allocated cores within a NUMA domain shared with other jobs')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not os.environ.get('SLURM_JOB_ID'):
        raise RuntimeError('Must run inside a Slurm compute allocation')
    allowed = set(os.sched_getaffinity(0))
    online = cpu_list(Path('/sys/devices/system/cpu/online').read_text())
    nodes = []
    for node in Path('/sys/devices/system/node').glob('node[0-9]*'):
        nodes.append((int(node.name[4:]), cpu_list((node / 'cpulist').read_text())))
    if args.node is not None:
        nodes = [(node, cpus) for node, cpus in nodes if node == args.node]
    if args.threads is not None and args.threads < 1:
        parser.error('--threads must be positive')
    node, cpus = choose_domain(allowed, online, nodes, args.threads or 1, args.allow_shared)
    topology = {}
    for cpu in cpus:
        path = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        topology[cpu] = tuple(int((path / name).read_text())
                              for name in ('physical_package_id', 'core_id'))
    binding = physical_cpus(cpus, topology)
    threads = args.threads or len(binding)
    if threads > len(binding):
        raise RuntimeError('Requested threads exceed the physical cores in the owned NUMA domain')
    metadata = dict(hostname=os.uname().nodename, numa_node=node, numa_cpus=cpus,
                    allocated_affinity=sorted(allowed), matching_threads=threads,
                    physical_core_cpus=binding, baseline='unity-only',
                    slurm_job=os.environ['SLURM_JOB_ID'],
                    topology=subprocess.check_output(['lscpu', '--json'], text=True),
                    slurm_allocation=subprocess.check_output(['scontrol', 'show', 'job', os.environ['SLURM_JOB_ID']], text=True))
    metadata['isolation'] = 'shared-numa' if args.allow_shared else 'full-numa'
    metadata['whole_domain_owned'] = next(full & online for number, full in nodes if number == node) <= allowed
    Path(args.metadata).write_text(json.dumps(metadata, indent=2) + '\n')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        raise RuntimeError('Missing command')
    command = benchmark_command(command, threads)
    print(f'NUMA domain {node}: allocated CPUs {cpus}; {threads} matching threads; {metadata["isolation"]}', flush=True)
    os.execvp('numactl', ['numactl', '--physcpubind=' + ','.join(map(str, binding)),
                         f'--membind={node}', *command])


if __name__ == '__main__':
    main()
