#!/bin/bash -l
# sbatch --exclusive --nodes=1 --ntasks=1 --cpus-per-task=64
#        --constraint=intel8352y --mem=32G --no-requeue
set -euo pipefail
root=/home/juelinliu_umass_edu/GraphMini
module load conda/latest
conda activate "$root/env"
exec python "$root/source/scripts/unity_node.py"
