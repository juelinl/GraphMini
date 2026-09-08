import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from unity_numa import cpu_list, choose_domain, physical_cpus, benchmark_command
from unity_node import partition_patterns


class NumaTest(unittest.TestCase):
    def test_shared_domain_selects_allocated_subset(self):
        nodes = [(0, set(range(32))), (1, set(range(32, 64)))]
        self.assertEqual(choose_domain(set(range(24, 56)), set(range(64)), nodes, 1, True),
                         (1, list(range(32, 56))))

    def test_shared_domain_does_not_use_unallocated_cpus(self):
        self.assertEqual(choose_domain({2, 3, 33, 34}, set(range(64)),
                         [(0, set(range(32))), (1, set(range(32, 64)))], 1, True), (0, [2, 3]))

    def test_worker_patterns_are_disjoint_and_complete(self):
        self.assertEqual(partition_patterns([117, 158, 207, 208], 2), [[117, 207], [158, 208]])
        groups = [partition_patterns(list(range(112))[2*i:2*i+2], 2) for i in range(56)]
        self.assertEqual([v for groupset in groups for group in groupset for v in group], list(range(112)))

    def test_physical_cores_exclude_smt(self):
        self.assertEqual(physical_cpus([0, 1, 2, 3],
                         {0: (0, 0), 1: (0, 1), 2: (0, 0), 3: (0, 1)}), [0, 1])

    def test_socket_ids_disambiguate_cores(self):
        self.assertEqual(physical_cpus([0, 1], {0: (0, 0), 1: (1, 0)}), [0, 1])

    def test_full_domain_thread_argument(self):
        self.assertEqual(benchmark_command(['python', 'run.py', '--threads', '{numa_threads}'], 64),
                         ['python', 'run.py', '--threads', '64'])

    def test_parse(self):
        self.assertEqual(cpu_list('0-3,8,10-11\n'), {0, 1, 2, 3, 8, 10, 11})

    def test_full_domain(self):
        nodes = [(0, set(range(16))), (1, set(range(16, 32)))]
        self.assertEqual(choose_domain(set(range(32)), set(range(32)), nodes, 12),
                         (0, list(range(16))))

    def test_fragmented_allocation_rejected(self):
        nodes = [(0, set(range(16))), (1, set(range(16, 32)))]
        with self.assertRaises(RuntimeError):
            choose_domain(set(range(8)) | set(range(16, 24)), set(range(32)), nodes, 12)

    def test_offline_cpus_excluded(self):
        self.assertEqual(choose_domain(set(range(12)), set(range(12)),
                                       [(0, set(range(16)))], 12), (0, list(range(12))))


if __name__ == '__main__':
    unittest.main()
