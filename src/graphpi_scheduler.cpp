#include "graphpi_scheduler.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <set>
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

class RankClass {
public:
    RankClass(std::vector<int> order,
              std::vector<int> in_degrees,
              std::vector<int> weighted_in_degrees,
              std::vector<int> out_degrees)
            : order_(std::move(order)),
              in_degrees_(std::move(in_degrees)),
              weighted_in_degrees_(std::move(weighted_in_degrees)),
              out_degrees_(std::move(out_degrees)) {}

    const std::vector<int> &get_order() const {
        return order_;
    }

    bool operator<(const RankClass &other) const {
        if (in_degrees_ != other.in_degrees_) {
            return in_degrees_ > other.in_degrees_;
        }
        if (weighted_in_degrees_ != other.weighted_in_degrees_) {
            return weighted_in_degrees_ > other.weighted_in_degrees_;
        }
        if (out_degrees_ != other.out_degrees_) {
            return out_degrees_ > other.out_degrees_;
        }
        return order_ > other.order_;
    }

private:
    std::vector<int> order_;
    std::vector<int> in_degrees_;
    std::vector<int> weighted_in_degrees_;
    std::vector<int> out_degrees_;
};

class PatternGraph {
public:
    PatternGraph(const char *adj_mat, int size) : size_(size), adj_mat_(size * size, 0) {
        for (int i = 0; i < size_ * size_; ++i) {
            adj_mat_[i] = (adj_mat[i] == '1') ? 1 : 0;
        }
        for (int i = 0; i < size_; ++i) {
            adj_mat_[index_of(i, i, size_)] = 0;
        }
    }

    std::vector<int> get_reordered_adj_mat() {
        auto order = get_order();
        std::vector<int> rank(size_, 0);
        for (int i = 0; i < size_; ++i) {
            rank[order[i]] = i;
        }

        std::vector<int> reordered(size_ * size_, 0);
        for (int i = 0; i < size_; ++i) {
            for (int j = 0; j < size_; ++j) {
                reordered[index_of(rank[i], rank[j], size_)] = adj_mat_[index_of(i, j, size_)];
            }
        }
        return reordered;
    }

private:
    void get_full_permutation(std::vector<std::vector<int>> &vec,
                              std::vector<bool> &used,
                              std::vector<int> &tmp,
                              int depth) const {
        if (depth == size_) {
            vec.push_back(tmp);
            return;
        }
        for (int i = 0; i < size_; ++i) {
            if (!used[i]) {
                used[i] = true;
                tmp.push_back(i);
                get_full_permutation(vec, used, tmp, depth + 1);
                tmp.pop_back();
                used[i] = false;
            }
        }
    }

    int out_degree(int v_idx, const std::vector<bool> &used) const {
        int res = 0;
        for (int j = 0; j < size_; ++j) {
            if (!used[j] && j != v_idx) {
                res += adj_mat_[index_of(v_idx, j, size_)];
            }
        }
        return res;
    }

    int in_degree(int v_idx, const std::vector<bool> &used) const {
        int res = 0;
        for (int j = 0; j < size_; ++j) {
            if (used[j] && j != v_idx) {
                res += adj_mat_[index_of(j, v_idx, size_)];
            }
        }
        return res;
    }

    int weighted_in_degree(int v_idx, const std::vector<bool> &used, const std::vector<int> &rank) const {
        int res = 0;
        for (int j = 0; j < size_; ++j) {
            if (used[j]) {
                const auto pos = static_cast<int>(std::find(rank.begin(), rank.end(), j) - rank.begin());
                res += adj_mat_[index_of(j, v_idx, size_)] * (1 << (size_ - pos));
            }
        }
        return res;
    }

    std::vector<int> in_degrees(const std::vector<int> &rank) const {
        std::vector<int> result;
        std::vector<bool> used(size_, false);
        for (int r: rank) {
            used[r] = true;
            result.push_back(in_degree(r, used));
        }
        return result;
    }

    std::vector<int> weighted_in_degrees(const std::vector<int> &rank) const {
        std::vector<int> result;
        std::vector<bool> used(size_, false);
        for (int r: rank) {
            used[r] = true;
            result.push_back(weighted_in_degree(r, used, rank));
        }
        return result;
    }

    std::vector<int> out_degrees(const std::vector<int> &rank) const {
        std::vector<int> result;
        std::vector<bool> used(size_, false);
        for (int r: rank) {
            used[r] = true;
            result.push_back(out_degree(r, used));
        }
        return result;
    }

    std::vector<int> get_order() const {
        std::vector<std::vector<int>> ranks;
        std::vector<bool> used(size_, false);
        std::vector<int> rank;
        get_full_permutation(ranks, used, rank, 0);

        std::vector<RankClass> rank_classes;
        rank_classes.reserve(ranks.size());
        for (const auto &cur_rank: ranks) {
            rank_classes.emplace_back(cur_rank,
                                      in_degrees(cur_rank),
                                      weighted_in_degrees(cur_rank),
                                      out_degrees(cur_rank));
        }
        std::sort(rank_classes.begin(), rank_classes.end());
        return rank_classes.front().get_order();
    }

    int size_{0};
    std::vector<int> adj_mat_;
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
        if (!used[i]) {
            used[i] = true;
            tmp.push_back(i);
            get_full_permutation_for_size(size, vec, used, tmp, depth + 1);
            tmp.pop_back();
            used[i] = false;
        }
    }
}

std::vector<std::vector<int>> remove_automorphisms(const char *adj_mat,
                                                   int size,
                                                   const std::vector<std::vector<int>> &orders) {
    std::vector<std::string> all_mat;
    all_mat.reserve(orders.size());
    for (const auto &order: orders) {
        std::vector<int> rank(size, 0);
        for (int i = 0; i < size; ++i) {
            rank[order[i]] = i;
        }
        std::string local(size * size, '0');
        for (int i = 0; i < size; ++i) {
            for (int j = 0; j < size; ++j) {
                local[rank[i] * size + rank[j]] = adj_mat[i * size + j];
            }
        }
        all_mat.push_back(local);
    }

    std::vector<std::string> unique = all_mat;
    std::sort(unique.begin(), unique.end(), std::greater<std::string>());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    std::vector<std::vector<int>> out;
    out.reserve(unique.size());
    for (const auto &mat: unique) {
        const auto it = std::find(all_mat.begin(), all_mat.end(), mat);
        assert(it != all_mat.end());
        out.push_back(orders[static_cast<size_t>(std::distance(all_mat.begin(), it))]);
    }
    return out;
}

bool satisfies_restricts(const std::vector<int> &perm, const std::vector<std::pair<int, int>> &restricts) {
    for (const auto &[first, second]: restricts) {
        if (perm[first] <= perm[second]) {
            return false;
        }
    }
    return true;
}

} // namespace

void GraphPiScheduler::reset() {
    size_ = 0;
    total_prefix_num_ = 0;
    total_restrict_num_ = 0;
    in_exclusion_optimize_num_ = 0;
    in_exclusion_optimize_redundancy_ = 1;
    adj_mat_.clear();
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

std::string GraphPiScheduler::get_adj_mat_str() const {
    std::string out(static_cast<size_t>(size_ * size_), '0');
    for (int i = 0; i < size_; ++i) {
        for (int j = 0; j < size_; ++j) {
            out[index_of(i, j, size_)] = adj_mat_[index_of(i, j, size_)] == 0 ? '0' : '1';
        }
    }
    return out;
}

int GraphPiScheduler::get_max_degree() const {
    int max_degree = 0;
    for (int i = 0; i < size_; ++i) {
        int degree = 0;
        for (int j = 0; j < size_; ++j) {
            degree += adj_mat_[index_of(i, j, size_)];
        }
        max_degree = std::max(max_degree, degree);
    }
    return std::max(max_degree, 1);
}

void GraphPiScheduler::build_loop_invariant() {
    loop_set_prefix_id_.assign(size_, -1);
    for (int i = 1; i < size_; ++i) {
        std::vector<int> data;
        for (int j = 0; j < i; ++j) {
            if (adj_mat_[index_of(i, j, size_)] != 0) {
                data.push_back(j);
            }
        }
        loop_set_prefix_id_[i] = find_father_prefix(data);
    }
}

int GraphPiScheduler::find_father_prefix(const std::vector<int> &data) {
    if (data.empty()) {
        return -1;
    }

    const int num = data.back();
    for (int prefix_id = last_[num]; prefix_id != -1; prefix_id = next_[prefix_id]) {
        if (prefix_[prefix_id].equal(data)) {
            return prefix_id;
        }
    }

    std::vector<int> parent_data(data.begin(), data.end() - 1);
    const int father = find_father_prefix(parent_data);
    father_prefix_id_[total_prefix_num_] = father;
    next_[total_prefix_num_] = last_[num];
    last_[num] = total_prefix_num_;
    prefix_[total_prefix_num_].data = data;
    ++total_prefix_num_;
    return total_prefix_num_ - 1;
}

void GraphPiScheduler::add_restrict(const std::vector<std::pair<int, int>> &restricts) {
    restrict_pair = restricts;
    for (size_t i = 0; i < restrict_pair.size();) {
        bool keep = true;
        for (size_t j = 0; j < restrict_pair.size(); ++j) {
            if (i != j && restrict_pair[j].first == restrict_pair[i].first) {
                for (size_t k = 0; k < restrict_pair.size(); ++k) {
                    if (i != k && j != k &&
                        restrict_pair[k].second == restrict_pair[i].second &&
                        restrict_pair[j].second == restrict_pair[k].first) {
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
            restrict_pair.erase(restrict_pair.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }

    std::fill(restrict_last_.begin(), restrict_last_.end(), -1);
    std::fill(restrict_next_.begin(), restrict_next_.end(), -1);
    total_restrict_num_ = 0;
    for (const auto &p: restrict_pair) {
        restrict_index_[total_restrict_num_] = p.first;
        restrict_next_[total_restrict_num_] = restrict_last_[p.second];
        restrict_last_[p.second] = total_restrict_num_;
        ++total_restrict_num_;
    }
}

std::vector<std::vector<int>> GraphPiScheduler::get_isomorphism_vec() const {
    std::vector<std::vector<int>> perms;
    std::vector<bool> used(size_, false);
    std::vector<int> tmp;
    get_full_permutation_for_size(size_, perms, used, tmp, 0);

    std::vector<std::vector<int>> out;
    for (const auto &perm: perms) {
        bool is_iso = true;
        for (int i = 0; i < size_ && is_iso; ++i) {
            for (int j = i + 1; j < size_; ++j) {
                if (adj_mat_[index_of(i, j, size_)] != 0 &&
                    adj_mat_[index_of(perm[i], perm[j], size_)] == 0) {
                    is_iso = false;
                    break;
                }
            }
        }
        if (is_iso) {
            out.push_back(perm);
        }
    }
    return out;
}

std::vector<std::vector<int>> GraphPiScheduler::calc_permutation_group(const std::vector<int> &vec, int size) {
    std::vector<bool> used(static_cast<size_t>(size), false);
    std::vector<std::vector<int>> out;
    for (int i = 0; i < size; ++i) {
        if (!used[i]) {
            std::vector<int> group;
            int x = i;
            while (!used[x]) {
                used[x] = true;
                group.push_back(x);
                x = vec[x];
            }
            out.push_back(group);
        }
    }
    return out;
}

void GraphPiScheduler::aggressive_optimize_dfs(
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
            test_dag.add_ordered_edge(isomorphism_vec[i][first], isomorphism_vec[i][second]);
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

void GraphPiScheduler::aggressive_optimize_get_all_pairs(
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

void GraphPiScheduler::restricts_generate(const std::vector<int> &cur_adj_mat,
                                          std::vector<std::vector<std::pair<int, int>>> &restricts) const {
    GraphPiScheduler schedule;
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

int GraphPiScheduler::get_vec_optimize_num(const std::vector<int> &vec) const {
    bool is_valid = true;
    for (int i = 1; i < size_; ++i) {
        bool have_edge = false;
        for (int j = 0; j < i; ++j) {
            if (adj_mat_[index_of(vec[i], vec[j], size_)] != 0) {
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
            if (adj_mat_[index_of(vec[size_ - k], vec[i], size_)] != 0) {
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

void GraphPiScheduler::remove_invalid_permutation(std::vector<std::vector<int>> &candidate_permutations) const {
    for (size_t i = 0; i < candidate_permutations.size();) {
        const auto &vec = candidate_permutations[i];
        bool valid = true;
        for (int x = 1; x < size_; ++x) {
            bool have_edge = false;
            for (int y = 0; y < x; ++y) {
                if (adj_mat_[index_of(vec[x], vec[y], size_)] != 0) {
                    have_edge = true;
                    break;
                }
            }
            if (!have_edge) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            candidate_permutations.erase(candidate_permutations.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

double GraphPiScheduler::our_estimate_schedule_restrict(const std::vector<int> &order,
                                                        const std::vector<std::pair<int, int>> &pairs,
                                                        uint64_t v_cnt,
                                                        uint64_t e_cnt,
                                                        uint64_t tri_cnt) const {
    const int max_degree = get_max_degree();
    std::vector<double> p_size(max_degree, 1.0);
    std::vector<double> pp_size(max_degree, 1.0);

    const double p0 = static_cast<double>(e_cnt) / static_cast<double>(v_cnt) / static_cast<double>(v_cnt);
    const double p1 = static_cast<double>(tri_cnt) * static_cast<double>(v_cnt) /
                      static_cast<double>(e_cnt) / static_cast<double>(e_cnt);

    p_size[0] = static_cast<double>(v_cnt);
    for (int i = 1; i < max_degree; ++i) {
        p_size[i] = p_size[i - 1] * p0;
        pp_size[i] = pp_size[i - 1] * p1;
    }

    std::vector<int> rank(size_, 0);
    for (int i = 0; i < size_; ++i) {
        rank[order[i]] = i;
    }

    std::vector<int> cur_adj_mat(size_ * size_, 0);
    for (int i = 0; i < size_; ++i) {
        for (int j = 0; j < size_; ++j) {
            cur_adj_mat[index_of(rank[i], rank[j], size_)] = adj_mat_[index_of(i, j, size_)];
        }
    }

    auto restricts = pairs;
    std::sort(restricts.begin(), restricts.end());
    std::vector<double> sum(restricts.size(), 0.0);

    std::vector<int> tmp(size_, 0);
    std::iota(tmp.begin(), tmp.end(), 0);
    do {
        for (size_t i = 0; i < restricts.size(); ++i) {
            if (tmp[restricts[i].first] > tmp[restricts[i].second]) {
                sum[i] += 1.0;
            } else {
                break;
            }
        }
    } while (std::next_permutation(tmp.begin(), tmp.end()));

    double total = 1.0;
    for (int i = 2; i <= size_; ++i) {
        total *= static_cast<double>(i);
    }
    for (double &val: sum) {
        val /= total;
    }
    for (int i = static_cast<int>(sum.size()) - 1; i > 0; --i) {
        sum[static_cast<size_t>(i)] /= sum[static_cast<size_t>(i - 1)];
    }

    std::vector<std::vector<int>> invariant_size(static_cast<size_t>(size_));
    double val = 1.0;
    for (int i = size_ - 1; i >= 0; --i) {
        int cnt_forward = 0;
        for (int j = 0; j < i; ++j) {
            if (cur_adj_mat[index_of(j, i, size_)] != 0) {
                ++cnt_forward;
            }
        }
        int c = cnt_forward;
        for (int j = i - 1; j >= 0; --j) {
            if (cur_adj_mat[index_of(j, i, size_)] != 0) {
                invariant_size[static_cast<size_t>(j)].push_back(c--);
            }
        }

        for (int degree: invariant_size[static_cast<size_t>(i)]) {
            if (degree > 1) {
                val += p_size[1] * pp_size[static_cast<size_t>(degree - 2)] + p_size[1];
            }
        }
        val += 1.0;
        for (size_t j = 0; j < restricts.size(); ++j) {
            if (restricts[j].second == i) {
                val *= sum[j];
            }
        }
        val *= (i == 0) ? p_size[0] : p_size[1] * pp_size[static_cast<size_t>(cnt_forward - 1)];
    }
    return val;
}

double GraphPiScheduler::graphzero_estimate_schedule_restrict(const std::vector<int> &order,
                                                              const std::vector<std::pair<int, int>> &pairs,
                                                              uint64_t v_cnt,
                                                              uint64_t e_cnt) const {
    const int max_degree = get_max_degree();
    std::vector<double> p_size(max_degree, 1.0);
    const double p = static_cast<double>(e_cnt) / static_cast<double>(v_cnt) / static_cast<double>(v_cnt);
    p_size[0] = static_cast<double>(v_cnt);
    for (int i = 1; i < max_degree; ++i) {
        p_size[i] = p_size[i - 1] * p;
    }

    std::vector<int> rank(size_, 0);
    for (int i = 0; i < size_; ++i) {
        rank[order[i]] = i;
    }

    std::vector<int> cur_adj_mat(size_ * size_, 0);
    for (int i = 0; i < size_; ++i) {
        for (int j = 0; j < size_; ++j) {
            cur_adj_mat[index_of(rank[i], rank[j], size_)] = adj_mat_[index_of(i, j, size_)];
        }
    }

    auto restricts = pairs;
    std::sort(restricts.begin(), restricts.end());
    std::vector<double> sum(restricts.size(), 0.0);

    std::vector<int> tmp(size_, 0);
    std::iota(tmp.begin(), tmp.end(), 0);
    do {
        for (size_t i = 0; i < restricts.size(); ++i) {
            if (tmp[restricts[i].first] > tmp[restricts[i].second]) {
                sum[i] += 1.0;
            } else {
                break;
            }
        }
    } while (std::next_permutation(tmp.begin(), tmp.end()));

    double total = 1.0;
    for (int i = 2; i <= size_; ++i) {
        total *= static_cast<double>(i);
    }
    for (double &val: sum) {
        val /= total;
    }
    for (int i = static_cast<int>(sum.size()) - 1; i > 0; --i) {
        sum[static_cast<size_t>(i)] /= sum[static_cast<size_t>(i - 1)];
    }

    std::vector<std::vector<int>> invariant_size(static_cast<size_t>(size_));
    double val = 1.0;
    for (int i = size_ - 1; i >= 0; --i) {
        int cnt_forward = 0;
        for (int j = 0; j < i; ++j) {
            if (cur_adj_mat[index_of(j, i, size_)] != 0) {
                ++cnt_forward;
            }
        }
        int c = cnt_forward;
        for (int j = i - 1; j >= 0; --j) {
            if (cur_adj_mat[index_of(j, i, size_)] != 0) {
                invariant_size[static_cast<size_t>(j)].push_back(c--);
            }
        }
        for (int degree: invariant_size[static_cast<size_t>(i)]) {
            if (degree > 1) {
                val += p_size[static_cast<size_t>(degree - 1)] + p_size[1];
            }
        }
        for (size_t j = 0; j < restricts.size(); ++j) {
            if (restricts[j].second == i) {
                val *= sum[j];
            }
        }
        val *= p_size[static_cast<size_t>(cnt_forward)];
    }
    return val;
}

void GraphPiScheduler::init_in_exclusion_optimize() {
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

void GraphPiScheduler::get_in_exclusion_optimize_group(int depth,
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

void GraphPiScheduler::set_in_exclusion_optimize_redundancy() {
    // GraphMini only needs a stable redundancy factor to compile and generate code.
    // The original GraphPi implementation estimates this by running its matcher on a
    // synthetic complete graph, which drags in the full graph/MPI stack. Keep the
    // standalone scheduler self-contained by falling back to the non-optimized factor.
    in_exclusion_optimize_redundancy_ = 1;
}

void GraphPiScheduler::get_schedule(const char *input_adj_mat,
                                    int input_size,
                                    uint64_t v_cnt,
                                    uint64_t e_cnt,
                                    uint64_t tri_cnt,
                                    PerfModelType model_type) {
    reset();
    size_ = input_size;
    adj_mat_.assign(static_cast<size_t>(size_ * size_), 0);

    std::vector<int> original_adj_mat(static_cast<size_t>(size_ * size_), 0);
    for (int i = 0; i < size_ * size_; ++i) {
        const int value = (input_adj_mat[i] == '1') ? 1 : 0;
        adj_mat_[static_cast<size_t>(i)] = value;
        original_adj_mat[static_cast<size_t>(i)] = value;
    }

    std::vector<std::vector<int>> candidate_permutations;
    std::vector<bool> used(static_cast<size_t>(size_), false);
    std::vector<int> tmp;
    get_full_permutation_for_size(size_, candidate_permutations, used, tmp, 0);
    candidate_permutations = remove_automorphisms(input_adj_mat, size_, candidate_permutations);
    remove_invalid_permutation(candidate_permutations);

    int max_val = 0;
    for (const auto &vec: candidate_permutations) {
        max_val = std::max(max_val, get_vec_optimize_num(vec));
    }

    std::vector<std::vector<int>> filtered_candidates;
    for (const auto &vec: candidate_permutations) {
        if (get_vec_optimize_num(vec) == max_val) {
            filtered_candidates.push_back(vec);
        }
    }
    candidate_permutations = std::move(filtered_candidates);

    std::vector<int> best_order(static_cast<size_t>(size_), 0);
    std::vector<std::pair<int, int>> best_pairs;
    double min_val = 0.0;
    bool have_best = false;

    for (const auto &vec: candidate_permutations) {
        std::vector<int> rank(static_cast<size_t>(size_), 0);
        for (int i = 0; i < size_; ++i) {
            rank[static_cast<size_t>(vec[static_cast<size_t>(i)])] = i;
        }

        std::vector<int> cur_adj_mat(static_cast<size_t>(size_ * size_), 0);
        for (int i = 0; i < size_; ++i) {
            for (int j = 0; j < size_; ++j) {
                cur_adj_mat[index_of(rank[static_cast<size_t>(i)],
                                     rank[static_cast<size_t>(j)],
                                     size_)] = adj_mat_[index_of(i, j, size_)];
            }
        }

        std::vector<std::vector<std::pair<int, int>>> restricts_vector;
        restricts_generate(cur_adj_mat, restricts_vector);
        if (restricts_vector.empty()) {
            restricts_vector.emplace_back();
        }

        for (const auto &pairs: restricts_vector) {
            const double val = (model_type == PerfModelType::graphpi)
                               ? our_estimate_schedule_restrict(vec, pairs, v_cnt, e_cnt, tri_cnt)
                               : graphzero_estimate_schedule_restrict(vec, pairs, v_cnt, e_cnt);
            if (!have_best || val < min_val) {
                have_best = true;
                min_val = val;
                best_order = vec;
                best_pairs = pairs;
            }
        }
    }

    std::vector<int> rank(static_cast<size_t>(size_), 0);
    for (int i = 0; i < size_; ++i) {
        rank[static_cast<size_t>(best_order[static_cast<size_t>(i)])] = i;
    }
    for (int i = 0; i < size_; ++i) {
        for (int j = 0; j < size_; ++j) {
            adj_mat_[index_of(rank[static_cast<size_t>(i)], rank[static_cast<size_t>(j)], size_)] =
                    original_adj_mat[index_of(i, j, size_)];
        }
    }

    restrict_pair = best_pairs;

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
            if (adj_mat_[index_of(i, j, size_)] != 0) {
                valid = true;
                break;
            }
        }
        assert(valid && "invalid schedule");
    }

    build_loop_invariant();
    add_restrict(best_pairs);
    set_in_exclusion_optimize_redundancy();
}

} // namespace minigraph
