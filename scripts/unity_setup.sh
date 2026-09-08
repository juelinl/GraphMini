#!/bin/bash -l
# Submit with sbatch --cpus-per-task=8 --mem=24G --time=02:00:00.
set -euo pipefail
root=/home/juelinliu_umass_edu/GraphMini
cd "$root"
module load conda/latest
export CONDA_PKGS_DIRS="$root/conda-pkgs"
if [[ ! -x "$root/env/bin/python" ]]; then
    conda env create --yes --prefix "$root/env" --file "$root/environment.yml"
fi
conda activate "$root/env"
conda install --yes --override-channels -c conda-forge numactl networkx
python --version
"$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-c++" --version
touch "$root/environment-ready"
