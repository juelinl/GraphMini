//
// Created by ubuntu on 1/1/23.
//

#pragma once

#include <cstddef>
#include <string>
#include "common/meta.h"
#include "compiler/config.h"
#include <bitset>
#include <optional>
#include <vector>
#include <cassert>
namespace minigraph {
    /*! \brief Maximum pattern size supported for generating pattern matching plan*/
    static const int MAX_PATTERN_SIZE = 64;
    /*! \brief Binary representation of an vertex's edges coming from other vertices in the pattern.
     * For a triangle pattern graph
     * Edges: (v0, v1), (v0, v2), (v1, v2)
     * v0's EdgeIR: 011
     * v1's EdgeIR: 101
     * v2's EdgeIR: 110
     */
    typedef std::bitset<MAX_PATTERN_SIZE> EdgeIR;

    /*! \brief Binary representation of a graph pattern.
     * For a triangle pattern graph (assuming edges are undirected)
     * Edges: (v0, v1), (v0, v2), (v1, v2)
     * MatrixIR: [011, 101, 110]
     */
    typedef std::vector<EdgeIR> MatrixIR;
    typedef EdgeIR EdgeRestrictIR;
    typedef MatrixIR MatrixRestrictIR;

    struct MiniGraphIR;

    class VertexSetIR;

    struct PlanIR;

    class VertexSetIR {
    private:
        EdgeIR m_edges;
        EdgeRestrictIR m_restricts;
        int m_loop_depth{-1}; // VertexIR should include all incoming-edges matched in [0, m_loop_depth]^th vertices
    public:
        int id{-1};
        AdjMatType adjMatType{AdjMatType::VertexInduced};
        VertexSetIR() = default;
        VertexSetIR(const EdgeIR &edges, const EdgeRestrictIR restricts, int loop_depth, AdjMatType query_type);

        // true if exactly the same
        bool operator==(const VertexSetIR &rhs) const;
        bool same_iep_computation(const VertexSetIR &rhs) const;

        bool
        operator<(const VertexSetIR &rhs) const;; // for sorting VertexSetIR in the order of 1) edge_num 2) restrict_num
        int loop_depth() const { return m_loop_depth; }; // m_loop_depth
        int edge_num() const { return m_edges.count(); }; // number of 1s in m_edges in the range of [0, loop_depth]
        int restrict_num() const { return m_restricts.count(); }; // number of 1s in m_restricts in the range of [0, loop_depth]
        bool share_at_least_one_parent_node(const VertexSetIR& other) const;
        bool is_restricted(int depth) const { return m_restricts[depth]; };
        bool is_edge(int depth) const { return m_edges[depth]; };
        bool has_id() const { return id != -1; };
        bool is_superset_of(const VertexSetIR &rhs) const;;
        friend struct MiniGraphIR;
        friend std::ostream &operator<<(std::ostream &out, const VertexSetIR &v);
        friend std::ostream &operator<<(std::ostream &out, const MiniGraphIR &mg);
    };

    struct MiniGraphIR {
        VertexSetIR m_vertices;
        VertexSetIR m_intersect;
        int id{-1};
        int vset_id() const {return m_vertices.id;};
        int vint_id() const {return m_intersect.id;};
        bool has_id() const { return id != -1; };
        int loop_depth() const {
            assert(m_intersect.loop_depth() == m_vertices.loop_depth());
            assert(m_vertices.loop_depth() >= 0);
            return m_vertices.loop_depth();
        }
        MiniGraphIR() = default;

        MiniGraphIR(const VertexSetIR &vertices, const VertexSetIR &intersect) : m_vertices{vertices}, m_intersect{intersect} {};

        bool operator==(const MiniGraphIR &rhs) const;;

        bool operator<(const MiniGraphIR &rhs) const;;

        bool is_superset_of(const MiniGraphIR &rhs) const;;

        bool is_superset_of(const VertexSetIR &vertices, const VertexSetIR &intersect) const;;

        // vs is already computed, so not need to compute again
        bool computed(const VertexSetIR &vertices, const VertexSetIR &intersect) const;;

        friend std::ostream &operator<<(std::ostream &out, const MiniGraphIR &mg);
    };

    // Query semantics are independent of scheduling, graph statistics, and storage.
    struct QueryIR {
        std::string adjacency;
        AdjMatType mode{AdjMatType::VertexInduced};
    };

    // Representation-independent constraints after choosing a matching order.
    struct ScheduledConstraints {
        int p_size{0};
        std::string adjacency;
        std::vector<int> matching_order;
        std::vector<std::vector<VertexSetIR>> set_ops;
        std::vector<VertexSetIR> iter_set;
    };

    struct PlanningContext {
        MetaData meta;
        CodeGenConfig config;
    };

    struct InclusionExclusionPlan {
        int iep_num{0}, iep_depth{0}, iep_redundancy{0};
        std::vector<int> iep_vals;
        std::vector<std::vector<std::vector<int>>> iep_groups;
        std::vector<VertexSetIR> iep_set;
    };

    struct AuxiliaryGraphPlan {
        std::vector<std::vector<MiniGraphIR>> mg_ops;
        std::vector<std::vector<MiniGraphIR>> mg_used;
        std::vector<bool> mg_bounded;
    };

    // Pipeline aggregate, not a purely logical IR. Analyses can take only the
    // component they need, without depending on auxiliary or physical decisions.
    struct PlanIR {
        QueryIR query;
        ScheduledConstraints logical;
        PlanningContext context;
        InclusionExclusionPlan counting;
        AuxiliaryGraphPlan auxiliary;
        std::optional<VertexSetIR> get_parent_vset(const VertexSetIR& vset) const;
        std::optional<VertexSetIR> get_parent_vset(const VertexSetIR& vset, size_t dep) const;
        std::optional<MiniGraphIR> get_parent_mg(const VertexSetIR& intersect) const;
        std::optional<MiniGraphIR> get_parent_mg(const MiniGraphIR& child) const;
        bool is_bounded(const MiniGraphIR &mg) const;
        bool is_bounded(const VertexSetIR &vset) const;
    };
} // NAMESPACE minigraph
