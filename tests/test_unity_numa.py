import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from unity_numa import cpu_list, choose_domain


class NumaTest(unittest.TestCase):
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
