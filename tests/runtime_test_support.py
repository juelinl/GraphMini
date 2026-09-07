"""Keep runtime compilation tests from leaving a modified tracked plan behind."""
import atexit
from pathlib import Path


def restore_generated_plan_at_exit():
    # Tests sharing this source tree must run serially, like compile_plan itself.
    path = Path(__file__).resolve().parents[1] / "src/codegen_output/plan.cpp"
    original = path.read_bytes()
    atexit.register(lambda: path.write_bytes(original))
