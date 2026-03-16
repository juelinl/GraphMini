#include "graph_builder.h"

#include "common.h"
#include "graph_loader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace minigraph {

namespace {

constexpr uint64_t kTriangleSampleSize = 100;

void require_or_throw(bool condition, const std::string &message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

void validate_csr(const GraphCSRData &data) {
    require_or_throw(data.indptr.size() >= 2, "CSR indptr must contain at least two elements");
    require_or_throw(data.indptr.front() == 0, "CSR indptr must start at zero");
    require_or_throw(data.indptr.back() == data.indices.size(),
                     "CSR indptr back() must match the indices length");

    for (size_t i = 1; i < data.indptr.size(); ++i) {
        require_or_throw(data.indptr[i - 1] <= data.indptr[i], "CSR indptr must be non-decreasing");
    }

    const uint64_t num_vertex = static_cast<uint64_t>(data.indptr.size() - 1);
    if (!data.offsets.empty()) {
        require_or_throw(data.offsets.size() == num_vertex, "CSR offsets length must match the vertex count");
    }
    if (!data.triangles.empty()) {
        require_or_throw(data.triangles.size() == num_vertex, "CSR triangles length must match the vertex count");
    }

    for (uint64_t vertex = 0; vertex < num_vertex; ++vertex) {
        const auto begin = data.indices.begin() + static_cast<long>(data.indptr[vertex]);
        const auto end = data.indices.begin() + static_cast<long>(data.indptr[vertex + 1]);
        require_or_throw(std::is_sorted(begin, end), "CSR adjacency lists must be sorted");
        require_or_throw(std::adjacent_find(begin, end) == end,
                         "CSR adjacency lists must not contain duplicate neighbors");
        if (!data.offsets.empty()) {
            require_or_throw(data.offsets[vertex] <= static_cast<uint64_t>(std::distance(begin, end)),
                             "CSR offset exceeds the adjacency list length");
        }
        for (auto it = begin; it != end; ++it) {
            require_or_throw(static_cast<uint64_t>(*it) < num_vertex,
                             "CSR indices contain an out-of-range vertex id");
            require_or_throw(static_cast<uint64_t>(*it) != vertex, "CSR graphs must not contain self loops");
            const uint64_t neighbor = static_cast<uint64_t>(*it);
            const auto reciprocal_begin = data.indices.begin() + static_cast<long>(data.indptr[neighbor]);
            const auto reciprocal_end = data.indices.begin() + static_cast<long>(data.indptr[neighbor + 1]);
            require_or_throw(std::binary_search(reciprocal_begin, reciprocal_end, static_cast<IdType>(vertex)),
                             "CSR graphs must be undirected: missing reverse edge " +
                             std::to_string(neighbor) + " -> " + std::to_string(vertex));
        }
    }
}

uint64_t max_degree_from_indptr(const std::vector<uint64_t> &indptr) {
    uint64_t max_degree = 0;
    for (size_t i = 1; i < indptr.size(); ++i) {
        max_degree = std::max(max_degree, indptr[i] - indptr[i - 1]);
    }
    return max_degree;
}

uint64_t count_sorted_intersection(const IdType *lhs_begin,
                                   const IdType *lhs_end,
                                   const IdType *rhs_begin,
                                   const IdType *rhs_end) {
    uint64_t count = 0;
    while (lhs_begin != lhs_end && rhs_begin != rhs_end) {
        if (*lhs_begin < *rhs_begin) {
            ++lhs_begin;
        } else if (*rhs_begin < *lhs_begin) {
            ++rhs_begin;
        } else {
            ++count;
            ++lhs_begin;
            ++rhs_begin;
        }
    }
    return count;
}

struct TriangleEstimate {
    std::vector<uint64_t> per_vertex_raw;
    uint64_t estimated_total{0};
    uint64_t max_raw{0};
    double sampled_average_degree{0.0};
};

TriangleEstimate estimate_triangle_stats_from_csr(const GraphCSRData &data) {
    const uint64_t num_vertex = static_cast<uint64_t>(data.indptr.size() - 1);
    TriangleEstimate estimate;
    estimate.per_vertex_raw.assign(static_cast<size_t>(num_vertex), 0);
    if (num_vertex == 0) {
        return estimate;
    }

    std::vector<uint64_t> order(static_cast<size_t>(num_vertex));
    std::iota(order.begin(), order.end(), 0);
    const uint64_t sample_size = std::min<uint64_t>(kTriangleSampleSize, num_vertex);
    std::partial_sort(order.begin(),
                      order.begin() + static_cast<long>(sample_size),
                      order.end(),
                      [&](uint64_t lhs, uint64_t rhs) {
                          const uint64_t lhs_degree = data.indptr[lhs + 1] - data.indptr[lhs];
                          const uint64_t rhs_degree = data.indptr[rhs + 1] - data.indptr[rhs];
                          if (lhs_degree != rhs_degree) {
                              return lhs_degree > rhs_degree;
                          }
                          return lhs < rhs;
                      });

    long double sample_sum = 0.0;
    long double sample_degree_sum = 0.0;
    for (uint64_t sample_index = 0; sample_index < sample_size; ++sample_index) {
        const uint64_t vertex = order[static_cast<size_t>(sample_index)];
        const auto *vertex_begin = data.indices.data() + data.indptr[vertex];
        const auto *vertex_end = data.indices.data() + data.indptr[vertex + 1];
        sample_degree_sum += static_cast<long double>(vertex_end - vertex_begin);

        uint64_t raw_triangle_count = 0;
        for (auto *neighbor_it = vertex_begin; neighbor_it != vertex_end; ++neighbor_it) {
            const uint64_t neighbor = static_cast<uint64_t>(*neighbor_it);
            const auto *neighbor_begin = data.indices.data() + data.indptr[neighbor];
            const auto *neighbor_end = data.indices.data() + data.indptr[neighbor + 1];
            raw_triangle_count += count_sorted_intersection(vertex_begin, vertex_end, neighbor_begin, neighbor_end);
        }

        estimate.per_vertex_raw[static_cast<size_t>(vertex)] = raw_triangle_count;
        estimate.max_raw = std::max(estimate.max_raw, raw_triangle_count);
        sample_sum += static_cast<long double>(raw_triangle_count);
    }

    const long double sample_average = sample_sum / static_cast<long double>(sample_size);
    estimate.sampled_average_degree = static_cast<double>(
            sample_degree_sum / static_cast<long double>(sample_size));
    const long double estimated_raw_total = sample_average * static_cast<long double>(num_vertex);
    estimate.estimated_total = static_cast<uint64_t>(std::llround(estimated_raw_total / 6.0L));
    return estimate;
}

} // namespace

std::vector<uint64_t> compute_offsets_from_csr(const std::vector<uint64_t> &indptr,
                                               const std::vector<IdType> &indices) {
    require_or_throw(indptr.size() >= 2, "CSR indptr must contain at least two elements");
    std::vector<uint64_t> offsets(indptr.size() - 1, 0);
    for (uint64_t vertex = 0; vertex + 1 < indptr.size(); ++vertex) {
        const auto begin = indices.begin() + static_cast<long>(indptr[vertex]);
        const auto end = indices.begin() + static_cast<long>(indptr[vertex + 1]);
        offsets[vertex] = static_cast<uint64_t>(std::lower_bound(begin, end, static_cast<IdType>(vertex)) - begin);
    }
    return offsets;
}

MetaData metadata_from_graph(const Graph &graph) {
    return MetaData(graph.num_vertex,
                    graph.num_edge,
                    graph.num_triangle,
                    graph.max_degree,
                    graph.max_offset,
                    std::max(graph.max_triangle, graph.max_degree),
                    graph.scheduler_avg_degree);
}

std::unique_ptr<Graph> load_graph_from_preprocessed(const std::filesystem::path &graph_dir,
                                                    bool reorder_by_degree) {
    GraphLoadOptions options;
    options.mmap = false;
    options.reorder_by_degree = reorder_by_degree;
    return std::unique_ptr<Graph>(load_bin<Graph>(graph_dir.string(), options));
}

std::unique_ptr<Graph> build_graph_from_csr(GraphCSRData data, bool reorder_by_degree) {
    validate_csr(data);

    if (data.offsets.empty()) {
        data.offsets = compute_offsets_from_csr(data.indptr, data.indices);
    }

    const uint64_t num_vertex = static_cast<uint64_t>(data.indptr.size() - 1);
    const uint64_t num_edge = static_cast<uint64_t>(data.indices.size());
    const uint64_t max_degree = max_degree_from_indptr(data.indptr);
    const uint64_t max_offset = data.offsets.empty() ? 0
                                                     : *std::max_element(data.offsets.begin(), data.offsets.end());
    const double global_average_degree = (num_vertex == 0)
                                         ? 0.0
                                         : static_cast<double>(num_edge) / static_cast<double>(num_vertex);

    uint64_t num_triangle = 0;
    uint64_t max_triangle = 0;
    double scheduler_avg_degree = global_average_degree;
    if (data.triangles.empty()) {
        TriangleEstimate estimate = estimate_triangle_stats_from_csr(data);
        data.triangles = std::move(estimate.per_vertex_raw);
        num_triangle = estimate.estimated_total;
        max_triangle = estimate.max_raw;
        scheduler_avg_degree = estimate.sampled_average_degree;
    } else {
        uint64_t raw_triangle_total = 0;
        for (uint64_t triangle_count: data.triangles) {
            raw_triangle_total += triangle_count;
            max_triangle = std::max(max_triangle, triangle_count);
        }
        require_or_throw(raw_triangle_total % 6 == 0,
                         "CSR triangles must follow GraphMini's raw counting convention; the total must be divisible by 6");
        num_triangle = raw_triangle_total / 6;
    }
    // get_maxdeg() currently returns max_triangle, so keep the runtime capacity safe
    max_triangle = std::max(max_triangle, max_degree);

    auto graph = std::make_unique<Graph>();
    graph->m_mmap = false;
    graph->num_vertex = num_vertex;
    graph->num_edge = num_edge;
    graph->num_triangle = num_triangle;
    graph->max_degree = max_degree;
    graph->max_offset = max_offset;
    graph->max_triangle = max_triangle;
    graph->scheduler_avg_degree = scheduler_avg_degree;

    graph->m_indptr = new uint64_t[num_vertex + 1];
    std::copy(data.indptr.begin(), data.indptr.end(), graph->m_indptr);
    graph->m_indices = new IdType[num_edge];
    std::copy(data.indices.begin(), data.indices.end(), graph->m_indices);
    graph->m_offset = new uint64_t[num_vertex];
    std::copy(data.offsets.begin(), data.offsets.end(), graph->m_offset);
    graph->m_triangles = new uint64_t[num_vertex];
    std::copy(data.triangles.begin(), data.triangles.end(), graph->m_triangles);

    if (reorder_by_degree) {
        reorder_graph_by_degree(*graph);
    }
    return graph;
}

} // namespace minigraph
