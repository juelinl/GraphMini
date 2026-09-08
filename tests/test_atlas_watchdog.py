import json
from pathlib import Path
import sys
import tempfile
import unittest
import struct

from benchmark_atlas_runtime import run_job, read_progress


class WatchdogTest(unittest.TestCase):
    def test_progress_format(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'progress.bin'
            raw = bytearray(64 + 2*64 + 3)
            struct.pack_into('=4Q', raw, 0, 1, 2, 3, 1)
            struct.pack_into('=q', raw, 64, 13)
            struct.pack_into('=q', raw, 128, 29)
            raw[-3:] = bytes([2, 1, 0])
            path.write_bytes(raw)
            snapshot = read_progress(path)
            self.assertEqual(snapshot['accumulated_matches'], 42)
            self.assertEqual(snapshot['completed_root_vertex_ids'], [0])
            self.assertEqual(snapshot['started_root_vertices'], 2)

    def test_timeout_keeps_progress(self):
        row = self.run_worker(
            "import struct; q=p.with_name('progress.bin'); "
            "raw=bytearray(129); struct.pack_into('=4Q',raw,0,1,1,1,1); "
            "struct.pack_into('=q',raw,64,42); raw[-1]=1; q.write_bytes(raw); "
            "p.write_text(json.dumps(dict(status='running', execution_started=time.monotonic(), "
            "progress_file=str(q), active_run='warmup'))); time.sleep(10)")
        self.assertEqual(row['status'], 'execution_timeout')
        self.assertEqual(row['partial_progress']['accumulated_matches'], 42)
        self.assertFalse(row['partial_progress']['is_final_count'])

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
