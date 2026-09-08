#!/bin/bash -l
set -euo pipefail
root=/home/juelinliu_umass_edu/GraphMini
module load conda/latest
conda activate "$root/env"
export CONDA_PKGS_DIRS="$root/conda-pkgs"
conda install --yes --override-channels -c conda-forge numactl networkx
cd "$root/source"
python scripts/install_onetbb.py --jobs 8 --prefix "$root/oneTBB" \
    --cc "$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-cc" \
    --cxx "$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-c++"
