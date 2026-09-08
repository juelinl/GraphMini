# Command-line guide

[Back to GraphMini](../README.md)

Activate your [build environment](setup.md) and run commands from the repository
root. These examples use `build`; substitute `build-conda` if needed.

## CLI Workflow

The CLI remains available for preprocessing and benchmark-style runs.

### Build the project

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

### Download and preprocess example datasets

```bash
bash dataset/download.sh
bash dataset/prep.sh
```

To preprocess a graph manually:

```bash
./build/bin/prep --path_to_graph=./dataset/wiki
```

### Run a single query from the CLI

```bash
./build/bin/run \
  --graph_name=wiki \
  --path_to_graph=./dataset/GraphMini/wiki \
  --query_name=P1 \
  --query_adjmat=0111101111011110 \
  --query_type=vertex \
  --pruning_type=costmodel \
  --parallel_type=nested_rt \
  --scheduler=graphmini \
  --num_threads=32 \
  --graph_reordering=true
```

Primary CLI options:

- `--graph_name`: graph nickname
- `--path_to_graph`: path to the preprocessed graph directory
- `--query_name`: query nickname
- `--query_adjmat`: flattened adjacency matrix string
- `--query_type`: `vertex`, `edge`, `edge_iep`
- `--pruning_type`: `none`, `static`, `eager`, `online`, `costmodel`
- `--parallel_type`: `openmp`, `tbb_top`, `nested`, `nested_rt`
- `--scheduler`: `graphpi`, `graphzero`, `graphmini`
- `--num_threads`: execution thread count
- `--graph_reordering`: enable or disable degree-based graph reordering
