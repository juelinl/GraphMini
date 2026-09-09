#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minigraph::metal {
using Adjacency = std::vector<std::vector<uint32_t>>;
struct Options {
    unsigned clique_size = 6; // Exact unlabeled K3..K8 counting only.
    size_t batch_bytes = 64 * 1024 * 1024;
    size_t scratch_bytes = 64 * 1024 * 1024;
    size_t max_tasks = 16384;
};
struct Result {
    uint64_t matches = 0, tasks = 0, regions = 0, batches = 0;
    double validation_seconds = 0, construction_seconds = 0;
    double dispatch_wait_seconds = 0, gpu_seconds = 0, total_seconds = 0;
    size_t peak_buffer_bytes = 0;
};
// Graph must be sorted, unique, undirected and loop-free. Checked on each call.
// Throws on unsupported query/oversized region/Metal failure; never drops work.
// A session caches pipelines. Construction/dispatch timing excludes session init.
class Backend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    Backend();
    ~Backend();
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
    std::string device_name() const;
    Result count_cliques(const Adjacency &graph, const Options &options = {});
};
} // namespace minigraph::metal
