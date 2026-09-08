#!/bin/bash -l
# Small, nonexclusive allocation. Both backends stay on the same host/domain.
set -euo pipefail
root=/home/juelinliu_umass_edu/GraphMini
module load conda/latest
conda activate "$root/env"
export GRAPHMINI_SHARED_NUMA=1
export GRAPHMINI_MATCH_THREADS=16
export GRAPHMINI_BUILD_JOBS=4
unset GRAPHMINI_NUMA_NODE
exec bash -l "$root/source/scripts/unity_benchmark.sh"
