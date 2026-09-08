"""Guard the benchmark's cross-revision comparisons against incomplete/bad data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def fixture(label):
    measurements = []
    for bitmap, seconds in [(False, 6), (True, 4 if label == "previous" else 2)]:
        measurements.append(dict(scheduler="outgoing", bitmap=bitmap, selected=bitmap,
                                 median_seconds=seconds, mad_seconds=0,
                                 code_sha256=label if bitmap else "same-array"))
    return [{"metadata": {"label": label}},
            dict(size=6, family="minus1", graph="fixture", threads=1, count=10,
                 pattern="fixture-pattern", graph_metadata={"vertices": 10, "edges": 20},
                 measurements=measurements)]


class AnalysisTests(unittest.TestCase):
    def run_analysis(self, current, previous=None):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for name, rows in [("previous", previous or fixture("previous")), ("current", current)]:
                if rows is None:
                    continue
                path = Path(directory) / (name + ".jsonl")
                path.write_text("".join(json.dumps(row) + "\n" for row in rows))
                paths.append(str(path))
            output = Path(directory) / "summary.json"
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("analyze_bitmap_server.py")),
                                     *paths, "--require-runtime-pairs", "--output", str(output)],
                                    capture_output=True, text=True)
            return result, json.loads(output.read_text()) if output.exists() else None

    def test_ratios(self):
        result, data = self.run_analysis(fixture("current"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(data["cases"][0]["refinement_speedup"], 2)
        self.assertEqual(data["cases"][0]["bitmap_vs_array"], 3)

    def test_count_mismatch(self):
        rows = fixture("current")
        rows[1]["count"] += 1
        self.assertNotEqual(self.run_analysis(rows)[0].returncode, 0)

    def test_graph_mismatch(self):
        rows = fixture("current")
        rows[1]["graph_metadata"]["edges"] += 1
        self.assertNotEqual(self.run_analysis(rows)[0].returncode, 0)

    def test_changed_array_control(self):
        rows = fixture("current")
        rows[1]["measurements"][0]["code_sha256"] = "changed"
        self.assertNotEqual(self.run_analysis(rows)[0].returncode, 0)

    def test_missing_revision(self):
        self.assertNotEqual(self.run_analysis(None)[0].returncode, 0)


if __name__ == "__main__":
    unittest.main()
