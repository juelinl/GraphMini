#pragma once

#include "common.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace minigraph {

struct GraphLoadOptions {
    bool mmap{false};
    bool reorder_by_degree{false};
    double *reindex_time_seconds{nullptr};
};

template<typename T>
inline void read_graph_file(const std::filesystem::path &path, T *&pointer, uint64_t num_elements) {
    CHECK(std::filesystem::is_regular_file(path)) << "File does not exists: " << path;
    const size_t num_bytes = sizeof(T) * num_elements;
    pointer = new T[num_elements];
    std::ifstream file(path, std::ios::binary | std::ios::in);
    file.read(reinterpret_cast<char *>(pointer), static_cast<std::streamsize>(num_bytes));
    CHECK(file.gcount() == static_cast<std::streamsize>(num_bytes))
            << "Only read " << ToReadableSize(file.gcount()) << " out of "
            << ToReadableSize(num_bytes) << " from " << path;
}

template<typename T>
inline void mmap_graph_file(const std::filesystem::path &path, T *&pointer, uint64_t num_elements) {
    CHECK(std::filesystem::is_regular_file(path)) << "File does not exists: " << path;
#ifdef _WIN32
    const auto path_w = path.wstring();
    HANDLE file_handle = CreateFileW(path_w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file_handle != INVALID_HANDLE_VALUE) << "Failed to open: " << path;
    HANDLE mapping_handle = CreateFileMappingW(file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CHECK(mapping_handle != nullptr) << "Failed to create file mapping: " << path;
    pointer = static_cast<T *>(MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, sizeof(T) * num_elements));
    CHECK(pointer != nullptr) << "Failed to map file: " << path;
    CHECK(CloseHandle(mapping_handle) != 0) << "Failed to close file mapping handle: " << path;
    CHECK(CloseHandle(file_handle) != 0) << "Failed to close file handle: " << path;
#else
    int fd = open(path.c_str(), O_RDONLY, 0);
    CHECK(fd != -1) << "Failed to open: " << path;
    pointer = static_cast<T *>(mmap(nullptr, sizeof(T) * num_elements, PROT_READ, MAP_SHARED, fd, 0));
    CHECK(pointer != MAP_FAILED) << "Failed to map file: " << path;
    CHECK(close(fd) == 0) << "Failed to close file: " << path;
#endif
}

template<typename T>
inline void release_graph_array(T *&pointer) {
    if (pointer != nullptr) {
        delete[] pointer;
        pointer = nullptr;
    }
}

template<typename GraphT>
inline void release_graph_indices(GraphT &graph) {
    using VertexId = std::remove_pointer_t<decltype(graph.m_indices)>;
    if (graph.m_indices == nullptr) {
        return;
    }
    if (graph.m_mmap) {
#ifdef _WIN32
        CHECK(UnmapViewOfFile(graph.m_indices) != 0) << "Failed to unmap graph indices";
#else
        munmap(graph.m_indices, sizeof(VertexId) * graph.num_edge);
#endif
    } else {
        delete[] graph.m_indices;
    }
    graph.m_indices = nullptr;
}

template<typename GraphT>
inline void reorder_graph_by_degree(GraphT &graph) {
    using VertexId = std::remove_pointer_t<decltype(graph.m_indices)>;
    if (graph.num_vertex == 0) {
        return;
    }

    std::vector<uint64_t> new_to_old(graph.num_vertex);
    std::iota(new_to_old.begin(), new_to_old.end(), uint64_t{0});
    std::sort(new_to_old.begin(), new_to_old.end(), [&graph](uint64_t lhs, uint64_t rhs) {
        const uint64_t lhs_degree = graph.m_indptr[lhs + 1] - graph.m_indptr[lhs];
        const uint64_t rhs_degree = graph.m_indptr[rhs + 1] - graph.m_indptr[rhs];
        if (lhs_degree != rhs_degree) {
            return lhs_degree > rhs_degree;
        }
        return lhs < rhs;
    });

    std::vector<VertexId> old_to_new(graph.num_vertex);
    for (uint64_t new_id = 0; new_id < graph.num_vertex; ++new_id) {
        old_to_new[new_to_old[new_id]] = static_cast<VertexId>(new_id);
    }

    auto *new_indptr = new uint64_t[graph.num_vertex + 1];
    new_indptr[0] = 0;
    for (uint64_t new_id = 0; new_id < graph.num_vertex; ++new_id) {
        const uint64_t old_id = new_to_old[new_id];
        new_indptr[new_id + 1] = new_indptr[new_id] + (graph.m_indptr[old_id + 1] - graph.m_indptr[old_id]);
    }

    auto *new_indices = new VertexId[graph.num_edge];
    auto *new_offset = new uint64_t[graph.num_vertex];
    auto *new_triangles = new uint64_t[graph.num_vertex];

    uint64_t max_offset = 0;
    for (uint64_t new_id = 0; new_id < graph.num_vertex; ++new_id) {
        const uint64_t old_id = new_to_old[new_id];
        const uint64_t start = graph.m_indptr[old_id];
        const uint64_t end = graph.m_indptr[old_id + 1];
        std::vector<VertexId> remapped_neighbors;
        remapped_neighbors.reserve(end - start);
        for (uint64_t idx = start; idx < end; ++idx) {
            remapped_neighbors.push_back(old_to_new[graph.m_indices[idx]]);
        }
        std::sort(remapped_neighbors.begin(), remapped_neighbors.end());
        std::copy(remapped_neighbors.begin(), remapped_neighbors.end(), new_indices + new_indptr[new_id]);
        const uint64_t offset = static_cast<uint64_t>(std::lower_bound(
                remapped_neighbors.begin(), remapped_neighbors.end(), static_cast<VertexId>(new_id))
                - remapped_neighbors.begin());
        new_offset[new_id] = offset;
        new_triangles[new_id] = graph.m_triangles[old_id];
        max_offset = std::max(max_offset, offset);
    }

    release_graph_indices(graph);
    release_graph_array(graph.m_indptr);
    release_graph_array(graph.m_offset);
    release_graph_array(graph.m_triangles);

    graph.m_indices = new_indices;
    graph.m_indptr = new_indptr;
    graph.m_offset = new_offset;
    graph.m_triangles = new_triangles;
    graph.max_offset = max_offset;
    graph.m_mmap = false;
}

template<typename VertexId>
inline std::filesystem::path indices_file_path(const std::string &in_dir) {
    std::filesystem::path indices_file = in_dir;
    if (sizeof(VertexId) == sizeof(uint64_t)) {
        indices_file /= Constant::kIndicesU64File;
    } else if (sizeof(VertexId) == sizeof(uint32_t)) {
        indices_file /= Constant::kIndicesU32File;
    } else {
        CHECK(false) << "unsupported IdType";
    }
    return indices_file;
}

template<typename GraphT>
inline GraphT *load_bin(const std::string &in_dir, GraphLoadOptions options = {}) {
    using VertexId = typename std::remove_pointer<decltype(std::declval<GraphT>().m_indices)>::type;
    GraphT *out = new GraphT;
    MetaData meta;
    meta.read(in_dir);

    out->m_mmap = options.mmap;
    out->num_vertex = meta.num_vertex;
    out->num_edge = meta.num_edge;
    out->num_triangle = meta.num_triangle;
    out->max_offset = meta.max_offset;
    out->max_degree = meta.max_degree;
    out->max_triangle = meta.max_triangle;

    read_graph_file<uint64_t>(std::filesystem::path{in_dir} / Constant::kIndptrU64File, out->m_indptr,
                              meta.num_vertex + 1);
    read_graph_file<uint64_t>(std::filesystem::path{in_dir} / Constant::kOffsetU64File, out->m_offset,
                              meta.num_vertex);
    read_graph_file<uint64_t>(std::filesystem::path{in_dir} / Constant::kTriangleU64File, out->m_triangles,
                              meta.num_vertex);

    const auto indices_file = indices_file_path<VertexId>(in_dir);
    if (options.mmap && !options.reorder_by_degree) {
        mmap_graph_file<VertexId>(indices_file, out->m_indices, meta.num_edge);
    } else {
        read_graph_file<VertexId>(indices_file, out->m_indices, meta.num_edge);
    }

    if (options.reindex_time_seconds != nullptr) {
        *options.reindex_time_seconds = 0.0;
    }
    if (options.reorder_by_degree) {
        Timer timer;
        reorder_graph_by_degree(*out);
        if (options.reindex_time_seconds != nullptr) {
            *options.reindex_time_seconds = timer.Passed();
        }
    }
    return out;
}

} // namespace minigraph
