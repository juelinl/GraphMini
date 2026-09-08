#!/bin/bash -l
# Request 64 physical CPUs on EPYC 7763, block placement. unity_numa.py
# verifies full NUMA-domain ownership before any measured work.
set -euo pipefail
root=/home/juelinliu_umass_edu/GraphMini
module load conda/latest
conda activate "$root/env"
export CONDA_PKGS_DIRS="$root/conda-pkgs"
export GRAPHMINI_BITMAP_TASK_POLICY=grain64
export CORPUS="$root/atlas6-corpus.json"
unset GRAPHMINI_PROGRESS_FILE
job_dir="$root/runs/${SLURM_ARRAY_JOB_ID:-$SLURM_JOB_ID}-${SLURM_ARRAY_TASK_ID:-pilot}"
mkdir -p "$job_dir"
git clone --no-hardlinks "$root/source" "$job_dir/repo"
cd "$job_dir/repo"
export PYTHONPATH="$job_dir/repo/build/lib"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-cc" \
    -DCMAKE_CXX_COMPILER="$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-c++" \
    -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python" \
    -DTBB_DIR="$root/oneTBB/lib/cmake/TBB" -DGRAPHMINI_BUILD_TESTS=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
python tests/test_atlas_watchdog.py
python tests/runtime_progress.py
python tests/test_unity_numa.py
if [[ -n "${SLURM_ARRAY_TASK_ID:-}" ]]; then
    atlas_ids=$(python -c 'import json,os; print(json.load(open(os.environ["CORPUS"]))["patterns"][int(os.environ["SLURM_ARRAY_TASK_ID"])]["atlas_id"])')
else
    atlas_ids=117,158,207,208
    # Exercise a real native cutoff before allowing the full array to start.
    python scripts/unity_numa.py --metadata "$job_dir/timeout-topology.json" --threads 12 -- \
        python tests/benchmark_atlas_runtime.py --corpus "$root/atlas6-corpus.json" \
        --output "$job_dir/timeout-check" --real-dir "$root/data" --atlas-ids 117 \
        --parallel nested_rt --threads 12 --execution-budget 0.5 --preparation-budget 600
    python - "$job_dir/timeout-check/results.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 2 and any(row['status'] == 'execution_timeout' for row in rows)
for row in rows:
    p = row['partial_progress']
    assert p['available'] and p['started_root_vertices'] > 0
    assert 0 <= p['accumulated_matches'] <= 486771082065
print('Native timeout snapshots verified for both backends')
PY
fi
python scripts/unity_numa.py --metadata "$job_dir/topology.json" --threads 12 -- \
    python tests/benchmark_atlas_runtime.py --corpus "$root/atlas6-corpus.json" \
    --output "$job_dir/results" --real-dir "$root/data" --atlas-ids "$atlas_ids" \
    --parallel nested_rt --threads 12 --execution-budget 300 --preparation-budget 600
