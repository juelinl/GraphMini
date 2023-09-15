# GraphMini
GraphMini is a high-performance graph pattern-matching system. It supports subgraph enumeration on arbitrary patterns. 

# Hardware Requirements
1. 128GB of free RAM to preprocess the graph Friendster correctly.
2. 180GB of free disk space to store preprocessed graphs.


# Software Dependencies
### Operating System
1. Ubuntu 22.04 (tested)

### Libraries
1. CMake (Version >= 3.20)
2. Make
3. MPI
4. GCC compiler (>= 7)
5. Python (>= 3.8)
6. bc
7. curl
8. clang-format (optional)

### Install libraries
```bash
sudo apt install curl bc cmake mpich clang-format -y
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

# How to run a single query with GraphMini
The binary takes 7 required and 1 optional input:
```bash
./build/bin/run [graph_name] [path_to_graph] [query_nickname] [query_adjmat] [query_type] [pruning_type] [parallel_type] [exp_id=-1 (optional)]
```
- graph_name: nickname for tested graph (ex. wiki)
- path_to_graph: path to the directory that contains the preprocessed graph. (ex ../Datasets/GraphMini/wiki)
- query_nickname: nickname for tested query (ex. P1)
- query_adjmat: adjacency matrix of the tested query (ex. "0111101111011110" 4-clique)
- query_type: 
    - 0: vertex-induced, 
    - 1: edge-induced, 
    - 2: edge-induced with IEP optimization
- pruning_type:
    - 0: None: not pruning
    - 1: Static: only prune adj that must be used in the future
    - 2: Eager: prune all adj that might be used in the future
    - 3: Online: lazily prune adj that is being queried
    - 4: CostModel: using cost model to decide which adj to prune
- parallel_type:
    - 0: OpenMP: parallel first loop only with OpenMP
    - 1: Tbb: parallel first loop only with Tbb
    - 2: Nested: nested loop for all computation
    - 3: NestedRt: nested loop + runtime information
- exp_id: experiment id (for generating logs)

For example:
```bash
./build/bin/run wiki ./dataset/GraphMini/wiki P1 0111101111011110 0 4 3
```

This query runs P1 (4clique) on graph wiki. The query is vertex-induced. The executable uses CostModel to decide which adjacency lists to prune and uses nested parallelism to speed up query execution. 
