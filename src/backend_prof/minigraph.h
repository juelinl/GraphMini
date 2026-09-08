//
// Created by ubuntu on 2/2/23.
//

#ifndef MINIGRAPH_MINIGRAPH_H
#define MINIGRAPH_MINIGRAPH_H

#include "vertex_set.h"
#include "graph.h"
#include "../backend/managed_container.h"
#include <atomic>
#include <tuple>

#define NOT_PRUNE -2
#define WILL_PRUNE -1

namespace minigraph {
    
    inline ManagedContainer get_indices(const VertexSet &_vertices, const VertexSet &_to_iter) {
        ManagedContainer out(_to_iter.size());
        out.set_size(set_ops::indices_write(_vertices.begin(), _vertices.size(),
                                            _to_iter.begin(), _to_iter.size(), out.begin()));
        return out;
    };


    // MiniGraph Interface
    struct MiniGraphIF {
        MiniGraphIF() = default;

        virtual ~MiniGraphIF() = default;

        inline static const Graph *DATA_GRAPH{nullptr};

        virtual void build(const VertexSet &_vertex,
                           const VertexSet &_intersect,
                           const VertexSet &_iter) = 0;

        virtual void build(MiniGraphIF *mg,
                           const VertexSet &_vertex,
                           const VertexSet &_intersect,
                           const VertexSet &_iter) = 0;

        virtual VertexSet N(IdType i) = 0;

        virtual ManagedContainer indices(const VertexSet &_to_iter) const = 0;

        virtual IdType Degree(IdType i) const = 0;
    };

    class MiniGraphEager : public MiniGraphIF {
    private:
        VertexSet m_vertex;
        VertexSet m_intersect;
        ManagedContainer m_ctn;
        ManagedContainer m_pos;
        ManagedContainer m_degree;
        ManagedContainer m_indices;
        const bool m_bounded{false};
        const bool m_par{false};
        size_t num_edges{0};
        MiniGraphIF *m_mg{nullptr};

    public:
        MiniGraphEager() = default;

        ~MiniGraphEager() override = default;

        MiniGraphEager(bool _bounded, bool _par = false) : m_bounded{_bounded}, m_par{_par} {};

        void build(const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_vertex = _vertex;
            m_intersect = _intersect;
            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = 0;
            m_pos[0] = num_edges;
            for (uint64_t i = 0; i < m_vertex.size(); ++i) {
                size_t buffer_required = m_intersect.size() + m_pos[i];
                if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                IdType v_id = m_vertex[i];
                IdType *start = m_ctn.begin() + m_pos[i];
                size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                          : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                m_degree[i] = degree;
                m_pos[i + 1] = m_pos[i] + degree;
                num_edges += degree;
            }
        }

        void build(MiniGraphIF *mg,
                   const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_mg = mg;
            m_vertex = _vertex;
            m_intersect = _intersect;
            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            m_pos[0] = 0;
            m_indices = m_mg->indices(m_vertex);
            for (uint64_t i = 0; i < m_vertex.size(); ++i) {
                size_t buffer_required = m_intersect.size() + m_pos[i];
                if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                IdType v_id = m_vertex[i];
                IdType adj_idx = m_indices[i];
                IdType *start = m_ctn.begin() + m_pos[i];
                size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                          : m_intersect.intersect(m_mg->N(adj_idx), start);
                m_degree[i] = degree;
                m_pos[i + 1] = m_pos[i] + degree;
            }
        }

        VertexSet N(IdType i) override {
            return VertexSet(m_vertex[i], m_ctn.begin() + m_pos[i], m_degree[i]);
        }

        ManagedContainer indices(const VertexSet &_to_iter) const override {
            auto out = get_indices(m_vertex, _to_iter);
            assert(out.size() == _to_iter.size());
            return out;
        }

        IdType Degree(IdType i) const override {
            return m_degree[i];
        }
    };

    class MiniGraphLazy : public MiniGraphIF {
    private:
        VertexSet m_vertex;
        VertexSet m_intersect;
        ManagedContainer m_ctn;
        ManagedContainer m_pos;
        ManagedContainer m_degree;
        ManagedContainer m_indices;
        ManagedContainer m_mg_indices;
        const bool m_bounded{false};
        const bool m_par{false};
        size_t num_edges{0};
        MiniGraphIF *m_mg{nullptr};

    public:
        MiniGraphLazy() = default;

        MiniGraphLazy(bool _bounded, bool _par = false) : m_bounded{_bounded}, m_par{_par} {};

        ~MiniGraphLazy() override = default;

        void build(const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_vertex = _vertex;
            m_intersect = _intersect;

            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = 0;
            m_pos[0] = 0;
            m_indices = get_indices(m_vertex, _iter);

            for (auto &x: m_degree) {
                x = INVALID_ID;
            }
            for (auto v_idx: m_indices) {
                m_pos[v_idx] = num_edges;
                size_t buffer_required = m_intersect.size() + num_edges;
                if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                IdType v_id = m_vertex[v_idx];
                IdType *start = m_ctn.begin() + m_pos[v_idx];
                size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                          : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                m_degree[v_idx] = degree;
                num_edges += degree;
            }
        }

        void build(MiniGraphIF *mg,
                   const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_mg = mg;
            m_vertex = _vertex;
            m_intersect = _intersect;
            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = 0;
            m_pos[0] = 0;
            for (auto &x: m_degree) {
                x = INVALID_ID;
            }
            m_indices = get_indices(m_vertex, _iter);
            m_mg_indices = m_mg->indices(m_vertex);
            assert(m_indices.size() == _iter.size());
            assert(m_mg_indices.size() == m_vertex.size());
            for (IdType v_idx: m_indices) {
                IdType v_id = m_vertex[v_idx];
                IdType adj_idx = m_mg_indices[v_idx];
                size_t buffer_required = m_intersect.size() + num_edges;
                if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                m_pos[v_idx] = num_edges;
                IdType *start = m_ctn.begin() + m_pos[v_idx];
                size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                          : m_intersect.intersect(m_mg->N(adj_idx), start);
                m_degree[v_idx] = degree;
                num_edges += degree;
            }
        }

        VertexSet N(IdType i) override {
            if (m_degree[i] == INVALID_ID) {
                if (m_mg) return m_mg->N(m_mg_indices[i]);
                else return DATA_GRAPH->N(m_vertex[i]);
            } else {
                return VertexSet(m_vertex[i], m_ctn.begin() + m_pos[i], m_degree[i]);
            }
        }

        IdType Degree(IdType i) const override {
            return m_degree[i];
        }

        ManagedContainer indices(const VertexSet &_to_iter) const override {
            auto out = get_indices(m_vertex, _to_iter);
            assert(out.size() == _to_iter.size());
            return out;
        }
    };


    class MiniGraphOnline : public MiniGraphIF {
    private:
        VertexSet m_vertex;
        VertexSet m_intersect;
        ManagedContainer m_ctn;
        ManagedContainer m_pos;
        ManagedContainer m_degree;
        ManagedContainer m_indices;
        ManagedContainer m_mg_indices;
        MiniGraphIF *m_mg{nullptr};
        const bool m_bounded{false};
        const bool m_par{false};
        size_t num_edges{0};
        size_t est_edges{0};
    public:
        MiniGraphOnline() = default;
        MiniGraphOnline(bool _bounded, bool _par = false) : m_bounded{_bounded}, m_par{_par} {};
        ~MiniGraphOnline() override = default;
        void build(const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_vertex = _vertex;
            m_intersect = _intersect;
            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = 0;
            m_pos[0] = 0;
            for (auto &x: m_degree) {
                x = INVALID_ID;
            }
            m_indices = get_indices(m_vertex, _iter);
            for (size_t i = 0; i < m_vertex.size(); i++) {
                IdType v_id = m_vertex[i];
                m_pos[i + 1] = m_bounded ? m_pos[i] + std::min(DATA_GRAPH->Offset(v_id), m_intersect.size())
                                         : m_pos[i] + std::min(DATA_GRAPH->Degree(v_id), m_intersect.size());
            }
            est_edges = m_pos[m_vertex.size()];
            if (m_ctn.capacity() < est_edges) m_ctn.Reserve(est_edges);
            for (auto v_idx: m_indices) {
                IdType v_id = m_vertex[v_idx];
                IdType *start = m_ctn.begin() + m_pos[v_idx];
                size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                          : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                m_degree[v_idx] = degree;
                num_edges += degree;
            }
        }

        void build(MiniGraphIF *mg,
                   const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_mg = mg;
            m_vertex = _vertex;
            m_intersect = _intersect;

            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = 0;

            for (auto &x: m_degree) {
                x = INVALID_ID;
            }

            m_indices = get_indices(m_vertex, _iter);
            m_mg_indices = m_mg->indices(m_vertex);
            assert(m_vertex.size() == m_mg_indices.size());
            assert(m_indices.size() <= _iter.size());
            m_pos[0] = 0;
            for (size_t i = 0; i < m_vertex.size(); i++) {
                IdType adj_idx = m_mg_indices[i];
                m_pos[i + 1] = m_pos[i] + std::min((IdType) m_intersect.size(), m_mg->Degree(adj_idx));
            }
            est_edges = m_pos[m_vertex.size()];
            if (m_ctn.capacity() < est_edges) m_ctn.Reserve(est_edges);
            for (IdType v_idx: m_indices) {
                IdType adj_idx = m_mg_indices[v_idx];
                IdType v_id = m_vertex[v_idx];
                IdType *start = m_ctn.begin() + m_pos[v_idx];
                size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                          : m_intersect.intersect(m_mg->N(adj_idx), start);
                m_degree[v_idx] = degree;
                num_edges += degree;
            }
        }

        VertexSet N(IdType i) override {
            if (m_degree[i] == INVALID_ID) {
                IdType v_id = m_vertex[i];
                if (m_mg != nullptr) {
                    IdType adj_idx = m_mg_indices[i];
                    IdType *start = m_ctn.begin() + m_pos[i];
                    size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                                : m_intersect.intersect(m_mg->N(adj_idx), start);
                    m_degree[i] = degree;
                } else {
                    IdType *start = m_ctn.begin() + m_pos[i];
                    size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                                : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                    m_degree[i] = degree;
                }
            }
            return VertexSet(m_vertex[i], m_ctn.begin() + m_pos[i], m_degree[i]);
        }

        IdType Degree(IdType i) const override {
            return m_degree[i];
        }

        ManagedContainer indices(const VertexSet &_to_iter) const override {
            auto out = get_indices(m_vertex, _to_iter);
            assert(out.size() == _to_iter.size());
            return out;
        }
    };
    class MiniGraphCostModel : public MiniGraphIF {
    private:
        VertexSet m_vertex;
        VertexSet m_intersect;
        ManagedContainer m_ctn;
        ManagedContainer m_pos;
        ManagedContainer m_degree;
        // ManagedContainer m_indices;
        ManagedContainer m_mg_indices;
        MiniGraphIF *m_mg{nullptr};
        const bool m_bounded{false};
        const bool m_par{false};
        size_t num_edges{0};
        size_t est_edges{0};
        size_t two_htop{0};
        // size_t threshold{0};
        double reuse_multiplier{0};
        // estimated number of times the pruned adj will be used
        // if less than 0 prune all

        bool should_prune(size_t idx) const {
            uint64_t degree = DATA_GRAPH->Degree(m_vertex[idx]);
            const static int large_degree_threshold = DATA_GRAPH->num_edge * 4 / DATA_GRAPH->num_vertex;
            if (degree >= large_degree_threshold) return true;
            auto intersect_size = m_intersect.size();
            double gain = reuse_multiplier / m_vertex.size() * (degree - 1.0 * intersect_size * degree / DATA_GRAPH->num_vertex) - intersect_size - degree;
            // double gain = reuse_multiplier / two_htop * degree * (degree - 1.0 * intersect_size * degree / DATA_GRAPH->num_vertex) - intersect_size - degree;
        //    if (gain < 0 && degree > 10000) {
        //        printf("Vertex Degree=%d | Reuse Factor=%f | Vertex Size=%d | Intersect Size=%d | Gain=%f\n ", degree, reuse_multiplier, m_vertex.size(), intersect_size, gain);
        //    }
            return gain > 0;
        }

    public:
        MiniGraphCostModel() = default;

        MiniGraphCostModel(bool _bounded, bool _par = false) : m_bounded{_bounded}, m_par{_par} {};

        ~MiniGraphCostModel() override = default;

        void set_reuse_multiplier(double _reuse) { reuse_multiplier = _reuse; };

        void build(const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_vertex = _vertex;
            m_intersect = _intersect;
            // threshold = DATA_GRAPH->get_enum() / DATA_GRAPH->get_vnum();
            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = two_htop = 0;
            m_pos[0] = 0;

            for (auto v_id : m_vertex) {
                two_htop += DATA_GRAPH->Degree(v_id);
            }
            if (_iter.begin() == m_vertex.begin()) {
                for (uint64_t i = 0; i < _iter.size(); ++i) {
                    size_t buffer_required = m_intersect.size() + m_pos[i];
                    if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                    IdType v_id = m_vertex[i];
                    IdType *start = m_ctn.begin() + m_pos[i];
                    size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                            : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                    m_degree[i] = degree;
                    m_pos[i + 1] = m_pos[i] + degree;
                    num_edges += degree;
                }
                for (uint64_t i = _iter.size(); i < m_vertex.size(); i++) {
                    if (should_prune(i)) {
                        // size_t buffer_required = m_intersect.size() + m_pos[i];
                        // if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        // IdType v_id = m_vertex[i];
                        // IdType *start = m_ctn.begin() + m_pos[i];
                        // size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                        //                         : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                        // m_degree[i] = degree;
                        // m_pos[i + 1] = m_pos[i] + degree;
                        // num_edges += degree;
                        IdType v_id = m_vertex[i];
                        size_t est_intersect_size = std::min(m_intersect.size(), DATA_GRAPH->Degree(v_id));
                        size_t buffer_required = est_intersect_size + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        m_pos[i+1] = m_pos[i] + est_intersect_size;
                        m_degree[i] = WILL_PRUNE;
                    } else {
                        m_degree[i] = NOT_PRUNE;
                        m_pos[i + 1] = m_pos[i];
                    };
                }
            } else {
                const IdType *iter_ptr = _iter.begin();
                for (uint64_t i = 0; i < m_vertex.size(); i++) {
                    IdType v_id = m_vertex[i];
                    iter_ptr = advance(iter_ptr, _iter.end(), v_id);
                    if (*iter_ptr == v_id) {
                        // can prune for free
                        size_t buffer_required = m_intersect.size() + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        IdType *start = m_ctn.begin() + m_pos[i];
                        size_t degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                                : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                        m_degree[i] = degree;
                        m_pos[i + 1] = m_pos[i] + degree;
                        num_edges += degree;
                    } else if (should_prune(i)) {
                        IdType v_id = m_vertex[i];
                        size_t est_intersect_size = std::min(m_intersect.size(), DATA_GRAPH->Degree(v_id));
                        size_t buffer_required = est_intersect_size + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        m_pos[i+1] = m_pos[i] + est_intersect_size;
                        m_degree[i] = WILL_PRUNE;
                    } else {
                        m_degree[i] = NOT_PRUNE;
                        m_pos[i + 1] = m_pos[i];
                    };
                }
            }
        }

        void build(MiniGraphIF *mg,
                   const VertexSet &_vertex,
                   const VertexSet &_intersect,
                   const VertexSet &_iter) override {
            m_mg = mg;
            m_vertex = _vertex;
            m_intersect = _intersect;

            if (m_pos.capacity() <= m_vertex.size()) {
                m_pos.Reserve(m_vertex.size() + 1024);
                m_degree.Reserve(m_vertex.size() + 1024);
            }
            m_pos.set_size(m_vertex.size() + 1);
            m_degree.set_size(m_vertex.size());
            num_edges = two_htop = 0;
            m_pos[0] = 0;

            for (auto v_id : m_vertex) {
                two_htop += DATA_GRAPH->Degree(v_id);
            }

            // m_indices = get_indices(m_vertex, _iter);
            m_mg_indices = m_mg->indices(m_vertex);
            assert(m_vertex.size() == m_mg_indices.size());
//            assert(m_indices.size() <= _iter.size());
            if (_iter.begin() == m_vertex.begin()) {
                for (uint64_t i = 0; i < _iter.size(); ++i) {
                    size_t buffer_required = m_intersect.size() + m_pos[i];
                    if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                    IdType v_id = m_vertex[i];
                    IdType adj_idx = m_mg_indices[i];
                    IdType *start = m_ctn.begin() + m_pos[i];

                    size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                            : m_intersect.intersect(m_mg->N(adj_idx), start);

                    m_degree[i] = degree;
                    m_pos[i + 1] = m_pos[i] + degree;
                    num_edges += degree;
                }
                for (uint64_t i = _iter.size(); i < m_vertex.size(); i++) {
                    if (should_prune(i)) {
                        // size_t buffer_required = m_intersect.size() + m_pos[i];
                        // if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        // IdType v_id = m_vertex[i];
                        // IdType *start = m_ctn.begin() + m_pos[i];
                        // IdType adj_idx = m_mg_indices[i];
                        // size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                        //                         : m_intersect.intersect(m_mg->N(adj_idx), start);
                        // m_degree[i] = degree;
                        // m_pos[i + 1] = m_pos[i] + degree;
                        // num_edges += degree;
                        IdType v_id = m_vertex[i];
                        size_t est_intersect_size = std::min(m_intersect.size(), DATA_GRAPH->Degree(v_id));
                        size_t buffer_required = est_intersect_size + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        m_pos[i+1] = m_pos[i] + est_intersect_size;
                        m_degree[i] = WILL_PRUNE;
                    } else {
                        m_degree[i] = NOT_PRUNE;
                        m_pos[i + 1] = m_pos[i];
                    };
                }
            } else {
                const IdType *iter_ptr = _iter.begin();
                for (uint64_t i = 0; i < m_vertex.size(); i++) {
                    IdType v_id = m_vertex[i];
                    iter_ptr = advance(iter_ptr, _iter.end(), v_id);
                    if (*iter_ptr == v_id) {
                        size_t buffer_required = m_intersect.size() + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        IdType *start = m_ctn.begin() + m_pos[i];
                        IdType adj_idx = m_mg_indices[i];
                        size_t degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                                : m_intersect.intersect(m_mg->N(adj_idx), start);
                        m_degree[i] = degree;
                        m_pos[i + 1] = m_pos[i] + degree;
                        num_edges += degree;
                    } else if (should_prune(i)) {
                        size_t est_intersect_size = std::min(m_intersect.size(), DATA_GRAPH->Degree(v_id));
                        size_t buffer_required = est_intersect_size + m_pos[i];
                        if (m_ctn.capacity() < buffer_required) m_ctn.Resize(buffer_required);
                        m_pos[i+1] = m_pos[i] + est_intersect_size;
                        m_degree[i] = WILL_PRUNE;
                    }
                    else {
                        m_degree[i] = NOT_PRUNE;
                        m_pos[i + 1] = m_pos[i];
                    };
                }
            }
        }

        VertexSet N(IdType i) override {
            if (m_degree[i] == NOT_PRUNE) {
                if (m_mg) {
                    return m_mg->N(m_mg_indices[i]);
                }
                else {
                    return DATA_GRAPH->N(m_vertex[i]);
                }
            } else if (m_degree[i] == WILL_PRUNE) {
                IdType v_id = m_vertex[i];
                IdType *start = m_ctn.begin() + m_pos[i];
                size_t degree{0};
                if (m_mg) {
                    IdType adj_idx = m_mg_indices[i];
                    degree = m_bounded ? m_intersect.intersect(m_mg->N(adj_idx), v_id, start)
                                            : m_intersect.intersect(m_mg->N(adj_idx), start);
                    m_degree[i] = degree;
                } else {
                    degree = m_bounded ? m_intersect.intersect(DATA_GRAPH->NBound(v_id), start)
                                            : m_intersect.intersect(DATA_GRAPH->N(v_id), start);
                    m_degree[i] = degree;
                }
                return VertexSet(v_id, start, degree);
            } else {
                return VertexSet(m_vertex[i], m_ctn.begin() + m_pos[i], m_degree[i]);
            }
        }

        IdType Degree(IdType i) const override {
            return m_degree[i];
        }

        ManagedContainer indices(const VertexSet &_to_iter) const override {
            auto out = get_indices(m_vertex, _to_iter);
            assert(out.size() == _to_iter.size());
            return out;
        }
    };
}
#endif //MINIGRAPH_MINIGRAPH_H
