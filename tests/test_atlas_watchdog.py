import json
from pathlib import Path
import sys
import tempfile
import unittest

from benchmark_atlas_runtime import run_job


class WatchdogTest(unittest.TestCase):
    def run_worker(self, script, execution=.2, preparation=2):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            prefix = ("import json,time; from pathlib import Path; "
                      f"p=Path({str(directory / 'state.json')!r}); ")
            return run_job([sys.executable, "-c", prefix + script], directory,
                           execution, preparation)

    def test_native_execution_timeout(self):
        row = self.run_worker("p.write_text(json.dumps(dict(status='running', "
                              "execution_started=time.monotonic()))); time.sleep(10)")
        self.assertEqual(row["status"], "execution_timeout")
        self.assertLess(row["job_wall_seconds"], 3)

    def test_preparation_timeout(self):
        row = self.run_worker("time.sleep(10)", preparation=.2)
        self.assertEqual(row["status"], "preparation_timeout")

    def test_completion(self):
        row = self.run_worker("p.write_text(json.dumps(dict(status='complete', count=7)))")
        self.assertEqual(row["status"], "complete")
        self.assertEqual(row["count"], 7)

    def test_failure(self):
        row = self.run_worker("raise RuntimeError('test')")
        self.assertEqual(row["status"], "error")
        self.assertNotEqual(row["returncode"], 0)


if __name__ == "__main__":
    unittest.main()
