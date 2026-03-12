#pragma once

#include "backend/graph.h"
#include "meta.h"
#include <filesystem>
#include <cstdint>
#include <memory>
#include <vector>

namespace minigraph {

struct GraphCSRData {
    std::vector<uint64_t> indptr;
    std::vector<IdType> indices;
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> triangles;
};

std::vector<uint64_t> compute_offsets_from_csr(const std::vector<uint64_t> &indptr,
                                               const std::vector<IdType> &indices);

MetaData metadata_from_graph(const Graph &graph);

std::unique_ptr<Graph> load_graph_from_preprocessed(const std::filesystem::path &graph_dir,
                                                    bool reorder_by_degree = false);

std::unique_ptr<Graph> build_graph_from_csr(GraphCSRData data, bool reorder_by_degree = false);

} // namespace minigraph
