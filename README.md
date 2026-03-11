# GraphMini
GraphMini is a high-performance graph pattern-matching system. It supports subgraph enumeration on arbitrary patterns. 

# Hardware Requirements
1. 128GB of free RAM to preprocess the graph Friendster correctly.
2. 180GB of free disk space to store preprocessed graphs.


# Software Dependencies
### Operating System
1. Ubuntu 22.04
2. Ubuntu 24.04

### Libraries
1. CMake (Version >= 3.20)
2. Make
3. GCC compiler (>= 7)
4. Python (>= 3.8)
5. bc
6. curl
7. clang-format (optional)

### Install libraries
```bash
sudo apt install curl bc cmake clang-format -y
```

# Tested Graph Data
1. Wiki
2. YouTube
3. Patents
4. LiveJournal
5. Orkut
6. Friendster

# How to compile
```bash
mkdir -p build && cd build && cmake .. && make -j
```

# How to download and preprocess data graph
After you successfully compile the code.

```bash
bash dataset/download.sh && bash dataset/prep.sh
```

To preprocess a graph manually, use:
```bash
./build/bin/prep --path_to_graph=./dataset/wiki
```

# How to run a single query with GraphMini
Run `./build/bin/run --help` for the full help text. The primary options are:
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
  --exp_id=42
```
- `--graph_name`: nickname for tested graph (example: `wiki`)
- `--path_to_graph`: path to the preprocessed graph directory (example: `./dataset/GraphMini/wiki`)
- `--query_name`: nickname for the tested query (example: `P1`)
- `--query_adjmat`: adjacency matrix of the tested query (example: `0111101111011110` for 4-clique)
- `--query_type`:
    - `vertex`: vertex-induced
    - `edge`: edge-induced
    - `edge_iep`: edge-induced with IEP optimization
- `--pruning_type`:
    - `none`: not pruning
    - `static`: only prune adj that must be used in the future
    - `eager`: prune all adj that might be used in the future
    - `online`: lazily prune adj that is being queried
    - `costmodel`: using cost model to decide which adj to prune
- `--parallel_type`:
    - `openmp`: parallel first loop only with OpenMP
    - `tbb_top`: parallel first loop only with Tbb
    - `nested`: nested loop for all computation
    - `nested_rt`: nested loop + runtime information
- `--scheduler`:
    - `graphpi`: GraphPi cost model scheduler
    - `graphzero`: GraphPi search with the GraphZero cost model
    - `graphmini`: deterministic greedy scheduler 
- `--num_threads`: positive thread count for execution; defaults to all available CPU threads
- `--exp_id`: experiment id for logging, optional

For example:
```bash
./build/bin/run \
  --graph_name=wiki \
  --path_to_graph=./dataset/GraphMini/wiki \
  --query_name=P1 \
  --query_adjmat=0111101111011110 \
  --query_type=vertex \
  --pruning_type=costmodel \
  --parallel_type=nested_rt \
  --scheduler=graphpi
```

This query runs `P1` on graph `wiki`. The query is vertex-induced, uses cost-model pruning, and uses nested runtime parallelism.
