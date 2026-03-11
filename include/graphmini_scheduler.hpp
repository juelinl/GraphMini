#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace minigraph {

class GraphMiniScheduler {
public:
    GraphMiniScheduler() = default;
    ~GraphMiniScheduler() = default;

    GraphMiniScheduler(const GraphMiniScheduler &) = delete;
    GraphMiniScheduler &operator=(const GraphMiniScheduler &) = delete;

    void get_schedule(const char *adj_mat,
                      int size,
                      uint64_t v_cnt = 0,
                      uint64_t e_cnt = 0,
                      uint64_t tri_cnt = 0);

    int get_size() const { return size_; }
    int get_in_exclusion_optimize_num() const { return in_exclusion_optimize_num_; }
    long long get_in_exclusion_optimize_redundancy() const { return in_exclusion_optimize_redundancy_; }
    std::string get_adj_mat_str() const;
    const std::vector<int> &get_matching_order() const { return matching_order_; }

    std::vector<std::vector<std::vector<int>>> in_exclusion_optimize_group;
    std::vector<int> in_exclusion_optimize_val;
    std::vector<std::pair<int, int>> restrict_pair;

private:
    struct PrefixRecord {
        std::vector<int> data;

        bool equal(const std::vector<int> &other) const { return data == other; }
    };

    void reset();
    void build_loop_invariant();
    int find_father_prefix(const std::vector<int> &data);
    void add_restrict(const std::vector<std::pair<int, int>> &restricts);

    std::vector<std::vector<int>> get_isomorphism_vec() const;
    static std::vector<std::vector<int>> calc_permutation_group(const std::vector<int> &vec, int size);
    void aggressive_optimize_get_all_pairs(std::vector<std::vector<std::pair<int, int>>> &ordered_pairs_vector) const;
    void aggressive_optimize_dfs(
            const std::vector<std::vector<int>> &isomorphism_vec,
            const std::vector<std::vector<std::vector<int>>> &permutation_groups,
            const std::vector<std::pair<int, int>> &ordered_pairs,
            std::vector<std::vector<std::pair<int, int>>> &ordered_pairs_vector) const;
    void restricts_generate(const std::vector<int> &cur_adj_mat,
                            std::vector<std::vector<std::pair<int, int>>> &restricts) const;
    int get_vec_optimize_num(const std::vector<int> &vec) const;
    void remove_invalid_permutation(std::vector<std::vector<int>> &candidate_permutations) const;
    void init_in_exclusion_optimize();
    void get_in_exclusion_optimize_group(int depth,
                                         std::vector<int> &id,
                                         int id_cnt,
                                         const std::vector<int> &in_exclusion_val);
    void set_in_exclusion_optimize_redundancy();

    int size_{0};
    int total_prefix_num_{0};
    int total_restrict_num_{0};
    int in_exclusion_optimize_num_{0};
    long long in_exclusion_optimize_redundancy_{1};

    std::vector<int> adj_mat_;
    std::vector<int> matching_order_;
    std::vector<int> father_prefix_id_;
    std::vector<int> last_;
    std::vector<int> next_;
    std::vector<int> loop_set_prefix_id_;
    std::vector<PrefixRecord> prefix_;
    std::vector<int> restrict_last_;
    std::vector<int> restrict_next_;
    std::vector<int> restrict_index_;
};

} // namespace minigraph
