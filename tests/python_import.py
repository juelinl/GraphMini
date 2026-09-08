"""Verify the renamed extension, not an unrelated installed module."""
from pathlib import Path
import sys

import graphmini as gm
import numpy as np

assert Path(gm.__file__).resolve() == Path(sys.argv[1]).resolve(), gm.__file__
assert gm.__name__ == "graphmini"
for name in ("Graph", "CompiledPlan", "RunResult"):
    assert getattr(gm, name).__module__ == "graphmini"
assert callable(gm.compile_plan)
graph = gm.Graph.from_csr(
    np.array([0, 2, 4, 6], dtype=np.uint64),
    np.array([1, 2, 0, 2, 0, 1], dtype=np.uint32),
)
assert graph.num_vertices == 3
assert graph.num_edges == 6
print(f"Validated graphmini import and CSR construction: {gm.__file__}")
