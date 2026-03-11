#include "graphmini_scheduler.hpp"
#include "logging.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/parallel_sort.h>
#include <string>
#include <utility>
#include <vector>

namespace minigraph {
namespace {

inline int index_of(int x, int y, int n) {
    return x * n + y;
}

class Pattern {
public:
    explicit Pattern(int size) : size_(size), adj_mat_(size * size, 0) {}

    void add_ordered_edge(int x, int y) {
        adj_mat_[index_of(x, y, size_)] = 1;
    }

    bool is_dag() const {
        std::vector<int> indegree(size_, 0);
        for (int i = 0; i < size_; ++i) {
            for (int j = 0; j < size_; ++j) {
                indegree[j] += adj_mat_[index_of(i, j, size_)];
            }
        }

        std::vector<int> queue;
        queue.reserve(size_);
        for (int i = 0; i < size_; ++i) {
            if (indegree[i] == 0) {
                queue.push_back(i);
            }
        }

        size_t head = 0;
        int seen = 0;
        while (head < queue.size()) {
            const int node = queue[head++];
            ++seen;
            for (int next = 0; next < size_; ++next) {
                if (adj_mat_[index_of(node, next, size_)] != 0) {
                    --indegree[next];
                    if (indegree[next] == 0) {
                        queue.push_back(next);
                    }
                }
            }
        }

        return seen == size_;
    }

private:
    int size_{0};
    std::vector<int> adj_mat_;
};

class DisjointSetUnion {
public:
    explicit DisjointSetUnion(int n) : parent_(n), set_size_(n) {
        init();
    }

    void init() {
        std::iota(parent_.begin(), parent_.end(), 0);
        set_size_ = static_cast<int>(parent_.size());
    }

    void merge(int a, int b) {
        const int fa = find(a);
        const int fb = find(b);
        if (fa != fb) {
            parent_[fa] = fb;
            --set_size_;
        }
    }

    int get_set_size() const {
        return set_size_;
    }

private:
    int find(int a) {
        if (parent_[a] == a) {
            return a;
        }
        parent_[a] = find(parent_[a]);
        return parent_[a];
    }

    std::vector<int> parent_;
    int set_size_{0};
};

void get_full_permutation_for_size(int size,
                                   std::vector<std::vector<int>> &vec,
                                   std::vector<bool> &used,
                                   std::vector<int> &tmp,
                                   int depth) {
    if (depth == size) {
        vec.push_back(tmp);
        return;
    }
    for (int i = 0; i < size; ++i) {
        if (!used[static_cast<size_t>(i)]) {
            used[static_cast<size_t>(i)] = true;
            tmp.push_back(i);
            get_full_permutation_for_size(size, vec, used, tmp, depth + 1);
            tmp.pop_back();
            used[static_cast<size_t>(i)] = false;
        }
    }
}

std::vector<std::vector<int>> get_all_permutations_for_size(int size) {
    std::vector<std::vector<std::vector<int>>> buckets(static_cast<size_t>(size));
    tbb::parallel_for(tbb::blocked_range<int>(0, size), [&](const tbb::blocked_range<int> &range) {
        for (int first = range.begin(); first != range.end(); ++first) {
            auto &bucket = buckets[static_cast<size_t>(first)];
            std::vector<bool> used(static_cast<size_t>(size), false);
            std::vector<int> tmp;
            tmp.reserve(static_cast<size_t>(size));
            used[static_cast<size_t>(first)] = true;
            tmp.push_back(first);
            get_full_permutation_for_size(size, bucket, used, tmp, 1);
        }
    });

    size_t total = 0;
    for (const auto &bucket: buckets) {
        total += bucket.size();
    }

    std::vector<std::vector<int>> out;
    out.reserve(total);
    for (auto &bucket: buckets) {
        out.insert(out.end(), bucket.begin(), bucket.end());
    }
    return out;
}

std::vector<int> build_rank(const std::vector<int> &order, int size) {
    std::vector<int> rank(static_cast<size_t>(size), 0);
    for (int i = 0; i < size; ++i) {
        rank[static_cast<size_t>(order[static_cast<size_t>(i)])] = i;
    }
    return rank;
}

std::vector<int> reorder_adj_mat(const std::vector<int> &adj_mat, const std::vector<int> &order, int size) {
    const auto rank = build_rank(order, size);
    std::vector<int> reordered(static_cast<size_t>(size * size), 0);
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            reordered[static_cast<size_t>(index_of(rank[static_cast<size_t>(i)],
                                                   rank[static_cast<size_t>(j)],
                                                   size))] = adj_mat[static_cast<size_t>(index_of(i, j, size))];
        }
    }
    return reordered;
}

std::string adj_mat_to_string(const std::vector<int> &adj_mat, int size) {
    std::string out(static_cast<size_t>(size * size), '0');
    for (int i = 0; i < size * size; ++i) {
        out[static_cast<size_t>(i)] = adj_mat[static_cast<size_t>(i)] == 0 ? '0' : '1';
    }
    return out;
}

uint64_t weight_for_position(int size, int position) {
    return uint64_t{1} << static_cast<unsigned>(size - 1 - position);
}

std::vector<uint64_t> build_score_vector(const std::vector<int> &adj_mat, int size) {
    std::vector<uint64_t> scores(static_cast<size_t>(size), 0);
    for (int i = 1; i < size; ++i) {
        uint64_t score = 0;
        for (int j = 0; j < i; ++j) {
            if (adj_mat[static_cast<size_t>(index_of(i, j, size))] != 0) {
                score += weight_for_position(size, j);
            }
        }
        scores[static_cast<size_t>(i)] = score;
    }
    return scores;
}

std::vector<int> build_restrict_adj_mat(const std::vector<std::pair<int, int>> &restricts, int size) {
    std::vector<int> adj_mat(static_cast<size_t>(size * size), 0);
    for (const auto &[first, second]: restricts) {
        adj_mat[static_cast<size_t>(index_of(first, second, size))] = 1;
        adj_mat[static_cast<size_t>(index_of(second, first, size))] = 1;
    }
    return adj_mat;
}

bool score_vector_better(const std::vector<uint64_t> &lhs,
                         const std::vector<uint64_t> &rhs) {
    return lhs > rhs;
}

std::vector<std::pair<int, int>> normalize_restricts(std::vector<std::pair<int, int>> restricts) {
    for (size_t i = 0; i < restricts.size();) {
        bool keep = true;
        for (size_t j = 0; j < restricts.size(); ++j) {
            if (i != j && restricts[j].first == restricts[i].first) {
                for (size_t k = 0; k < restricts.size(); ++k) {
                    if (i != k && j != k &&
                        restricts[k].second == restricts[i].second &&
                        restricts[j].second == restricts[k].first) {
                        keep = false;
                        break;
                    }
                }
            }
            if (!keep) {
                break;
            }
        }
        if (!keep) {
            restricts.erase(restricts.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    std::sort(restricts.begin(), restricts.end());
    return restricts;
}

std::vector<std::vector<int>> remove_automorphisms(const std::vector<int> &adj_mat,
                                                   int size,
                                                   const std::vector<std::vector<int>> &orders) {
    struct CanonicalOrder {
        std::string adj_key;
        size_t index{0};
    };

    std::vector<CanonicalOrder> all_mat(orders.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, orders.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
            const auto reordered = reorder_adj_mat(adj_mat, orders[i], size);
            all_mat[i].adj_key = adj_mat_to_string(reordered, size);
            all_mat[i].index = i;
        }
    });

    tbb::parallel_sort(all_mat.begin(), all_mat.end(), [](const CanonicalOrder &lhs, const CanonicalOrder &rhs) {
        if (lhs.adj_key != rhs.adj_key) {
            return lhs.adj_key < rhs.adj_key;
        }
        return lhs.index < rhs.index;
    });

    std::vector<std::vector<int>> out;
    out.reserve(all_mat.size());
    for (size_t i = 0; i < all_mat.size(); ++i) {
        if (i > 0 && all_mat[i].adj_key == all_mat[i - 1].adj_key) {
            continue;
        }
        out.push_back(orders[all_mat[i].index]);
    }
    return out;
}

bool satisfies_restricts(const std::vector<int> &perm, const std::vector<std::pair<int, int>> &restricts) {
    for (const auto &[first, second]: restricts) {
        if (perm[static_cast<size_t>(first)] <= perm[static_cast<size_t>(second)]) {
            return false;
        }
    }
    return true;
}

bool is_valid_permutation(const std::vector<int> &vec, const std::vector<int> &adj_mat, int size) {
    for (int x = 1; x < size; ++x) {
        bool have_edge = false;
        for (int y = 0; y < x; ++y) {
            if (adj_mat[static_cast<size_t>(index_of(vec[static_cast<size_t>(x)],
                                                     vec[static_cast<size_t>(y)],
                                                     size))] != 0) {
                have_edge = true;
                break;
            }
        }
        if (!have_edge) {
            return false;
        }
    }
    return true;
}

std::vector<int> find_isolated_vertices(const std::vector<int> &adj_mat, int size) {
    std::vector<int> isolated;
    for (int vertex = 0; vertex < size; ++vertex) {
        bool connected = false;
        for (int other = 0; other < size; ++other) {
            if (vertex == other) {
                continue;
            }
            if (adj_mat[static_cast<size_t>(index_of(vertex, other, size))] != 0 ||
                adj_mat[static_cast<size_t>(index_of(other, vertex, size))] != 0) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            isolated.push_back(vertex);
        }
    }
    return isolated;
}

std::string format_vertex_ids(const std::vector<int> &vertices) {
    std::string out;
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += "n" + std::to_string(vertices[i]);
    }
    return out;
}

struct RankedOrderCandidate {
    bool valid{false};
    std::vector<int> order;
    std::vector<int> reordered_adj_mat;
    std::vector<uint64_t> scores;
    std::string adj_key;
};

bool better_ranked_order(const RankedOrderCandidate &lhs, const RankedOrderCandidate &rhs) {
    if (!lhs.valid) {
        return false;
    }
    if (!rhs.valid) {
        return true;
    }
    if (score_vector_better(lhs.scores, rhs.scores)) {
        return true;
    }
    if (score_vector_better(rhs.scores, lhs.scores)) {
        return false;
    }
    if (lhs.adj_key != rhs.adj_key) {
        return lhs.adj_key > rhs.adj_key;
    }
    return lhs.order < rhs.order;
}

struct RankedRestrictCandidate {
    bool valid{false};
    std::vector<std::pair<int, int>> pairs;
    std::vector<uint64_t> scores;
};

bool better_ranked_restricts(const RankedRestrictCandidate &lhs, const RankedRestrictCandidate &rhs) {
    if (!lhs.valid) {
        return false;
    }
    if (!rhs.valid) {
        return true;
    }
    if (score_vector_better(lhs.scores, rhs.scores)) {
        return true;
    }
    if (score_vector_better(rhs.scores, lhs.scores)) {
        return false;
    }
    return lhs.pairs < rhs.pairs;
}

bool is_valid_prefix(const std::vector<int> &order, int depth, const std::vector<int> &adj_mat, int size) {
    if (depth == 0) {
        return true;
    }
    for (int pos = 1; pos <= depth; ++pos) {
        bool have_edge = false;
        for (int prev = 0; prev < pos; ++prev) {
            if (adj_mat[static_cast<size_t>(index_of(order[static_cast<size_t>(pos)],
                                                     order[static_cast<size_t>(prev)],
                                                     size))] != 0) {
                have_edge = true;
                break;
            }
        }
        if (!have_edge) {
            return false;
        }
    }
    return true;
}

void enumerate_best_order_dfs(const std::vector<int> &adj_mat,
                              int size,
                              std::vector<int> &order,
                              std::vector<bool> &used,
                              int depth,
                              RankedOrderCandidate &best) {
    if (depth == size) {
        RankedOrderCandidate candidate;
        candidate.valid = true;
        candidate.order = order;
        candidate.reordered_adj_mat = reorder_adj_mat(adj_mat, order, size);
        candidate.scores = build_score_vector(candidate.reordered_adj_mat, size);
        candidate.adj_key = adj_mat_to_string(candidate.reordered_adj_mat, size);
        if (better_ranked_order(candidate, best)) {
            best = std::move(candidate);
        }
        return;
    }

    for (int vertex = 0; vertex < size; ++vertex) {
        if (used[static_cast<size_t>(vertex)]) {
            continue;
        }
        order[static_cast<size_t>(depth)] = vertex;
        used[static_cast<size_t>(vertex)] = true;
        if (is_valid_prefix(order, depth, adj_mat, size)) {
            enumerate_best_order_dfs(adj_mat, size, order, used, depth + 1, best);
        }
        used[static_cast<size_t>(vertex)] = false;
    }
}

RankedOrderCandidate find_best_order_parallel(const std::vector<int> &adj_mat, int size) {
    return tbb::parallel_reduce(
            tbb::blocked_range<int>(0, size),
            RankedOrderCandidate{},
            [&](const tbb::blocked_range<int> &range, RankedOrderCandidate local_best) {
                std::vector<int> order(static_cast<size_t>(size), -1);
                std::vector<bool> used(static_cast<size_t>(size), false);
                for (int first = range.begin(); first != range.end(); ++first) {
                    std::fill(order.begin(), order.end(), -1);
                    std::fill(used.begin(), used.end(), false);
                    order[0] = first;
                    used[static_cast<size_t>(first)] = true;
                    enumerate_best_order_dfs(adj_mat, size, order, used, 1, local_best);
                }
                return local_best;
            },
            [](RankedOrderCandidate lhs, const RankedOrderCandidate &rhs) {
                if (better_ranked_order(rhs, lhs)) {
                    return rhs;
                }
                return lhs;
            });
}

bool is_automorphism(const std::vector<int> &adj_mat, const std::vector<int> &perm, int size) {
    for (int i = 0; i < size; ++i) {
        for (int j = i + 1; j < size; ++j) {
            if (adj_mat[static_cast<size_t>(index_of(i, j, size))] !=
                adj_mat[static_cast<size_t>(index_of(perm[static_cast<size_t>(i)],
                                                     perm[static_cast<size_t>(j)],
                                                     size))]) {
                return false;
            }
        }
    }
    return true;
}

void enumerate_automorphisms_dfs(const std::vector<int> &adj_mat,
                                 int size,
                                 std::vector<int> &perm,
                                 std::vector<bool> &used,
                                 int depth,
                                 std::vector<std::vector<int>> &out) {
    if (depth == size) {
        if (is_automorphism(adj_mat, perm, size)) {
            out.push_back(perm);
        }
        return;
    }

    for (int vertex = 0; vertex < size; ++vertex) {
        if (used[static_cast<size_t>(vertex)]) {
            continue;
        }
        perm[static_cast<size_t>(depth)] = vertex;
        used[static_cast<size_t>(vertex)] = true;
        enumerate_automorphisms_dfs(adj_mat, size, perm, used, depth + 1, out);
        used[static_cast<size_t>(vertex)] = false;
    }
}

} // namespace

void GraphMiniScheduler::reset() {
    size_ = 0;
    total_prefix_num_ = 0;
    total_restrict_num_ = 0;
    in_exclusion_optimize_num_ = 0;
    in_exclusion_optimize_redundancy_ = 1;
    adj_mat_.clear();
    matching_order_.clear();
    father_prefix_id_.clear();
    last_.clear();
    next_.clear();
    loop_set_prefix_id_.clear();
    prefix_.clear();
    restrict_last_.clear();
    restrict_next_.clear();
    restrict_index_.clear();
    in_exclusion_optimize_group.clear();
    in_exclusion_optimize_val.clear();
    restrict_pair.clear();
}

std::string GraphMiniScheduler::get_adj_mat_str() const {
    return adj_mat_to_string(adj_mat_, size_);
}

void GraphMiniScheduler::build_loop_invariant() {
    loop_set_prefix_id_.assign(static_cast<size_t>(size_), -1);
    for (int i = 1; i < size_; ++i) {
        std::vector<int> data;
        for (int j = 0; j < i; ++j) {
            if (adj_mat_[static_cast<size_t>(index_of(i, j, size_))] != 0) {
                data.push_back(j);
            }
        }
        loop_set_prefix_id_[static_cast<size_t>(i)] = find_father_prefix(data);
    }
}

int GraphMiniScheduler::find_father_prefix(const std::vector<int> &data) {
    if (data.empty()) {
        return -1;
    }

    const int num = data.back();
    for (int prefix_id = last_[static_cast<size_t>(num)]; prefix_id != -1;
         prefix_id = next_[static_cast<size_t>(prefix_id)]) {
        if (prefix_[static_cast<size_t>(prefix_id)].equal(data)) {
            return prefix_id;
        }
    }

    std::vector<int> parent_data(data.begin(), data.end() - 1);
    const int father = find_father_prefix(parent_data);
    father_prefix_id_[static_cast<size_t>(total_prefix_num_)] = father;
    next_[static_cast<size_t>(total_prefix_num_)] = last_[static_cast<size_t>(num)];
    last_[static_cast<size_t>(num)] = total_prefix_num_;
    prefix_[static_cast<size_t>(total_prefix_num_)].data = data;
    ++total_prefix_num_;
    return total_prefix_num_ - 1;
}

void GraphMiniScheduler::add_restrict(const std::vector<std::pair<int, int>> &restricts) {
    restrict_pair = normalize_restricts(restricts);

    std::fill(restrict_last_.begin(), restrict_last_.end(), -1);
    std::fill(restrict_next_.begin(), restrict_next_.end(), -1);
    total_restrict_num_ = 0;
    for (const auto &p: restrict_pair) {
        restrict_index_[static_cast<size_t>(total_restrict_num_)] = p.first;
        restrict_next_[static_cast<size_t>(total_restrict_num_)] =
                restrict_last_[static_cast<size_t>(p.second)];
        restrict_last_[static_cast<size_t>(p.second)] = total_restrict_num_;
        ++total_restrict_num_;
    }
}

std::vector<std::vector<int>> GraphMiniScheduler::get_isomorphism_vec() const {
    const int size = size_;
    const std::vector<int> adj_mat = adj_mat_;
    std::vector<std::vector<std::vector<int>>> buckets(static_cast<size_t>(size));
    tbb::parallel_for(tbb::blocked_range<int>(0, size), [&](const tbb::blocked_range<int> &range) {
        for (int first = range.begin(); first != range.end(); ++first) {
            auto &bucket = buckets[static_cast<size_t>(first)];
            std::vector<int> perm(static_cast<size_t>(size), -1);
            std::vector<bool> used(static_cast<size_t>(size), false);
            perm[0] = first;
            used[static_cast<size_t>(first)] = true;
            enumerate_automorphisms_dfs(adj_mat, size, perm, used, 1, bucket);
        }
    });

    std::vector<std::vector<int>> out;
    for (const auto &bucket: buckets) {
        out.insert(out.end(), bucket.begin(), bucket.end());
    }
    return out;
}

std::vector<std::vector<int>> GraphMiniScheduler::calc_permutation_group(const std::vector<int> &vec, int size) {
    std::vector<bool> used(static_cast<size_t>(size), false);
    std::vector<std::vector<int>> out;
    for (int i = 0; i < size; ++i) {
        if (!used[static_cast<size_t>(i)]) {
            std::vector<int> group;
            int x = i;
            while (!used[static_cast<size_t>(x)]) {
                used[static_cast<size_t>(x)] = true;
                group.push_back(x);
                x = vec[static_cast<size_t>(x)];
            }
            out.push_back(group);
        }
    }
    return out;
}

void GraphMiniScheduler::aggressive_optimize_dfs(
        const std::vector<std::vector<int>> &isomorphism_vec_in,
        const std::vector<std::vector<std::vector<int>>> &permutation_groups_in,
        const std::vector<std::pair<int, int>> &ordered_pairs_in,
        std::vector<std::vector<std::pair<int, int>>> &ordered_pairs_vector) const {
    auto isomorphism_vec = isomorphism_vec_in;
    auto permutation_groups = permutation_groups_in;
    auto ordered_pairs = ordered_pairs_in;

    Pattern base_dag(size_);
    for (const auto &[first, second]: ordered_pairs) {
        base_dag.add_ordered_edge(first, second);
    }

    for (size_t i = 0; i < isomorphism_vec.size();) {
        Pattern test_dag = base_dag;
        for (const auto &[first, second]: ordered_pairs) {
            test_dag.add_ordered_edge(isomorphism_vec[i][static_cast<size_t>(first)],
                                      isomorphism_vec[i][static_cast<size_t>(second)]);
        }
        if (!test_dag.is_dag()) {
            permutation_groups.erase(permutation_groups.begin() + static_cast<long>(i));
            isomorphism_vec.erase(isomorphism_vec.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }

    if (isomorphism_vec.size() == 1) {
        ordered_pairs_vector.push_back(ordered_pairs);
        return;
    }

    for (size_t i = 0; i < permutation_groups.size(); ++i) {
        for (const auto &group: permutation_groups[i]) {
            if (group.size() == 2) {
                const std::pair<int, int> found_pair(group[0], group[1]);
                auto next_groups = permutation_groups;
                auto next_iso = isomorphism_vec;
                auto next_pairs = ordered_pairs;
                next_groups.erase(next_groups.begin() + static_cast<long>(i));
                next_iso.erase(next_iso.begin() + static_cast<long>(i));
                next_pairs.push_back(found_pair);
                aggressive_optimize_dfs(next_iso, next_groups, next_pairs, ordered_pairs_vector);
                return;
            }
        }
    }
}

void GraphMiniScheduler::aggressive_optimize_get_all_pairs(
        std::vector<std::vector<std::pair<int, int>>> &ordered_pairs_vector) const {
    auto isomorphism_vec = get_isomorphism_vec();
    std::vector<std::vector<std::vector<int>>> permutation_groups;
    for (const auto &iso: isomorphism_vec) {
        permutation_groups.push_back(calc_permutation_group(iso, size_));
    }

    std::vector<std::pair<int, int>> ordered_pairs;
    for (size_t i = 0; i < permutation_groups.size();) {
        int two_element_count = 0;
        std::pair<int, int> found_pair;
        for (const auto &group: permutation_groups[i]) {
            if (group.size() == 2) {
                ++two_element_count;
                found_pair = {group[0], group[1]};
            } else if (group.size() != 1) {
                two_element_count = -1;
                break;
            }
        }
        if (two_element_count == 1) {
            permutation_groups.erase(permutation_groups.begin() + static_cast<long>(i));
            isomorphism_vec.erase(isomorphism_vec.begin() + static_cast<long>(i));
            ordered_pairs.push_back(found_pair);
        } else {
            ++i;
        }
    }

    aggressive_optimize_dfs(isomorphism_vec, permutation_groups, ordered_pairs, ordered_pairs_vector);
}

void GraphMiniScheduler::restricts_generate(const std::vector<int> &cur_adj_mat,
                                            std::vector<std::vector<std::pair<int, int>>> &restricts) const {
    GraphMiniScheduler schedule;
    schedule.size_ = size_;
    schedule.adj_mat_ = cur_adj_mat;
    restricts.clear();
    schedule.aggressive_optimize_get_all_pairs(restricts);

    const auto automorphisms = schedule.get_isomorphism_vec();
    for (size_t i = 0; i < restricts.size();) {
        int satisfied = 0;
        for (const auto &perm: automorphisms) {
            if (satisfies_restricts(perm, restricts[i])) {
                ++satisfied;
            }
        }
        if (satisfied != 1) {
            restricts.erase(restricts.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

int GraphMiniScheduler::get_vec_optimize_num(const std::vector<int> &vec) const {
    bool is_valid = true;
    for (int i = 1; i < size_; ++i) {
        bool have_edge = false;
        for (int j = 0; j < i; ++j) {
            if (adj_mat_[static_cast<size_t>(index_of(vec[static_cast<size_t>(i)],
                                                      vec[static_cast<size_t>(j)],
                                                      size_))] != 0) {
                have_edge = true;
                break;
            }
        }
        if (!have_edge) {
            is_valid = false;
            break;
        }
    }
    if (!is_valid) {
        return -1;
    }

    for (int k = 2; k <= size_; ++k) {
        bool ok = true;
        for (int i = size_ - k + 1; i < size_; ++i) {
            if (adj_mat_[static_cast<size_t>(index_of(vec[static_cast<size_t>(size_ - k)],
                                                      vec[static_cast<size_t>(i)],
                                                      size_))] != 0) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            return k - 1;
        }
    }

    return -1;
}

void GraphMiniScheduler::remove_invalid_permutation(std::vector<std::vector<int>> &candidate_permutations) const {
    std::vector<char> keep(candidate_permutations.size(), 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, candidate_permutations.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
                          for (size_t i = range.begin(); i != range.end(); ++i) {
                              keep[i] = is_valid_permutation(candidate_permutations[i], adj_mat_, size_) ? 1 : 0;
                          }
                      });

    std::vector<std::vector<int>> valid_permutations;
    valid_permutations.reserve(candidate_permutations.size());
    for (size_t i = 0; i < candidate_permutations.size(); ++i) {
        if (keep[i] != 0) {
            valid_permutations.push_back(candidate_permutations[i]);
        }
    }
    candidate_permutations = std::move(valid_permutations);
}

void GraphMiniScheduler::init_in_exclusion_optimize() {
    assert(in_exclusion_optimize_num_ >= 1);

    std::vector<int> in_exclusion_val(static_cast<size_t>(in_exclusion_optimize_num_ * 2), 0);
    for (int n = 1; n <= in_exclusion_optimize_num_; ++n) {
        in_exclusion_val[static_cast<size_t>(2 * n - 2)] = 0;
        in_exclusion_val[static_cast<size_t>(2 * n - 1)] = 0;

        if (n == 1) {
            ++in_exclusion_val[0];
            continue;
        }

        const int m = n * (n - 1) / 2;
        std::vector<std::pair<int, int>> edges;
        edges.reserve(static_cast<size_t>(m));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < i; ++j) {
                edges.emplace_back(i, j);
            }
        }

        DisjointSetUnion dsu(n);
        for (int mask = 0; mask < (1 << m); ++mask) {
            dsu.init();
            int bit_cnt = 0;
            for (int i = 0; i < m; ++i) {
                if ((mask & (1 << i)) != 0) {
                    ++bit_cnt;
                    dsu.merge(edges[static_cast<size_t>(i)].first, edges[static_cast<size_t>(i)].second);
                }
            }
            if (dsu.get_set_size() == 1) {
                ++in_exclusion_val[static_cast<size_t>(2 * n - ((bit_cnt & 1) ? 1 : 2))];
            }
        }
    }

    in_exclusion_optimize_group.clear();
    in_exclusion_optimize_val.clear();
    std::vector<int> ids(static_cast<size_t>(in_exclusion_optimize_num_), 0);
    get_in_exclusion_optimize_group(0, ids, 0, in_exclusion_val);
}

void GraphMiniScheduler::get_in_exclusion_optimize_group(int depth,
                                                         std::vector<int> &id,
                                                         int id_cnt,
                                                         const std::vector<int> &in_exclusion_val) {
    if (depth == in_exclusion_optimize_num_) {
        std::vector<int> size(static_cast<size_t>(id_cnt), 0);
        for (int i = 0; i < in_exclusion_optimize_num_; ++i) {
            ++size[static_cast<size_t>(id[static_cast<size_t>(i)])];
        }

        int even = in_exclusion_val[static_cast<size_t>(size[0] * 2 - 2)];
        int odd = in_exclusion_val[static_cast<size_t>(size[0] * 2 - 1)];
        for (int i = 1; i < id_cnt; ++i) {
            const int next_even = even * in_exclusion_val[static_cast<size_t>(size[static_cast<size_t>(i)] * 2 - 2)] +
                                  odd * in_exclusion_val[static_cast<size_t>(size[static_cast<size_t>(i)] * 2 - 1)];
            const int next_odd = even * in_exclusion_val[static_cast<size_t>(size[static_cast<size_t>(i)] * 2 - 1)] +
                                 odd * in_exclusion_val[static_cast<size_t>(size[static_cast<size_t>(i)] * 2 - 2)];
            even = next_even;
            odd = next_odd;
        }

        std::vector<std::vector<int>> group;
        for (int i = 0; i < id_cnt; ++i) {
            std::vector<int> cur;
            for (int j = 0; j < in_exclusion_optimize_num_; ++j) {
                if (id[static_cast<size_t>(j)] == i) {
                    cur.push_back(j);
                }
            }
            group.push_back(std::move(cur));
        }
        in_exclusion_optimize_group.push_back(std::move(group));
        in_exclusion_optimize_val.push_back(even - odd);
        return;
    }

    id[static_cast<size_t>(depth)] = id_cnt;
    get_in_exclusion_optimize_group(depth + 1, id, id_cnt + 1, in_exclusion_val);
    for (int i = 0; i < id_cnt; ++i) {
        id[static_cast<size_t>(depth)] = i;
        get_in_exclusion_optimize_group(depth + 1, id, id_cnt, in_exclusion_val);
    }
}

void GraphMiniScheduler::set_in_exclusion_optimize_redundancy() {
    in_exclusion_optimize_redundancy_ = 1;
}

void GraphMiniScheduler::get_schedule(const char *input_adj_mat,
                                      int input_size,
                                      uint64_t v_cnt,
                                      uint64_t e_cnt,
                                      uint64_t tri_cnt) {
    (void) v_cnt;
    (void) e_cnt;
    (void) tri_cnt;

    reset();
    size_ = input_size;
    adj_mat_.assign(static_cast<size_t>(size_ * size_), 0);

    std::vector<int> original_adj_mat(static_cast<size_t>(size_ * size_), 0);
    for (int i = 0; i < size_ * size_; ++i) {
        const int value = (input_adj_mat[i] == '1') ? 1 : 0;
        adj_mat_[static_cast<size_t>(i)] = value;
        original_adj_mat[static_cast<size_t>(i)] = value;
    }
    for (int i = 0; i < size_; ++i) {
        adj_mat_[static_cast<size_t>(index_of(i, i, size_))] = 0;
        original_adj_mat[static_cast<size_t>(index_of(i, i, size_))] = 0;
    }

    const std::vector<int> isolated_vertices = find_isolated_vertices(original_adj_mat, size_);
    CHECK(isolated_vertices.empty())
            << "Invalid query pattern: isolated vertices remain after removing diagonal self-loops: "
            << format_vertex_ids(isolated_vertices);

    const RankedOrderCandidate best_order = find_best_order_parallel(original_adj_mat, size_);
    CHECK(best_order.valid) << "Invalid schedule: no valid matching order exists for the input query pattern.";

    matching_order_ = best_order.order;
    adj_mat_ = best_order.reordered_adj_mat;

    std::vector<std::vector<std::pair<int, int>>> restricts_vector;
    restricts_generate(adj_mat_, restricts_vector);
    if (restricts_vector.empty()) {
        restricts_vector.emplace_back();
    }

    const RankedRestrictCandidate best_restricts = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, restricts_vector.size()),
            RankedRestrictCandidate{},
            [&](const tbb::blocked_range<size_t> &range, RankedRestrictCandidate local_best) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    RankedRestrictCandidate candidate;
                    candidate.valid = true;
                    candidate.pairs = normalize_restricts(restricts_vector[i]);
                    const auto restrict_adj_mat = build_restrict_adj_mat(candidate.pairs, size_);
                    candidate.scores = build_score_vector(restrict_adj_mat, size_);
                    if (better_ranked_restricts(candidate, local_best)) {
                        local_best = std::move(candidate);
                    }
                }
                return local_best;
            },
            [](RankedRestrictCandidate lhs, const RankedRestrictCandidate &rhs) {
                if (better_ranked_restricts(rhs, lhs)) {
                    return rhs;
                }
                return lhs;
            });
    restrict_pair = best_restricts.pairs;

    std::vector<int> identity(static_cast<size_t>(size_), 0);
    std::iota(identity.begin(), identity.end(), 0);
    in_exclusion_optimize_num_ = get_vec_optimize_num(identity);
    if (in_exclusion_optimize_num_ <= 1) {
        in_exclusion_optimize_num_ = 0;
    } else {
        --in_exclusion_optimize_num_;
        init_in_exclusion_optimize();
    }

    const int max_prefix_num = size_ * (size_ - 1) / 2;
    father_prefix_id_.assign(static_cast<size_t>(max_prefix_num), -1);
    last_.assign(static_cast<size_t>(size_), -1);
    next_.assign(static_cast<size_t>(max_prefix_num), -1);
    loop_set_prefix_id_.assign(static_cast<size_t>(size_), -1);
    prefix_.assign(static_cast<size_t>(max_prefix_num), {});
    restrict_last_.assign(static_cast<size_t>(size_), -1);
    restrict_next_.assign(static_cast<size_t>(max_prefix_num), -1);
    restrict_index_.assign(static_cast<size_t>(max_prefix_num), -1);

    total_prefix_num_ = 0;
    total_restrict_num_ = 0;

    for (int i = 1; i < size_; ++i) {
        bool valid = false;
        for (int j = 0; j < i; ++j) {
            if (adj_mat_[static_cast<size_t>(index_of(i, j, size_))] != 0) {
                valid = true;
                break;
            }
        }
        assert(valid && "invalid schedule");
    }

    build_loop_invariant();
    add_restrict(restrict_pair);
    set_in_exclusion_optimize_redundancy();
}

} // namespace minigraph
