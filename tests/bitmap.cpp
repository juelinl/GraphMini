#include "backend/bitgraph.h"
#include "backend/set_ops/set_ops.h"
#include "compiler/representation.h" // Runtime and compiler universe names must coexist.
#include <iostream>
#include <random>
#include <set>

using namespace minigraph;
namespace {
void require(bool valid, const char *message) {
    if (!valid)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected, "Invalid bitmap input was accepted");
}

void raw_kernels(std::mt19937_64 &rng) {
    for (size_t n : {0, 1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1025}) {
        const size_t nw = bit_ops::word_count(n);
        for (int trial = 0; trial < 50; ++trial) {
            std::vector<uint64_t> a(nw), b(nw), output(nw);
            for (size_t i = 0; i < nw; ++i) {
                a[i] = rng();
                b[i] = rng();
            }
            for (size_t limit : {size_t{0}, n / 2, n, n + 1}) {
                size_t intersection = 0, difference = 0, cardinality = 0;
                std::vector<size_t> positions;
                for (size_t i = 0; i < n; ++i) {
                    bool av = (a[i / 64] >> (i % 64)) & 1;
                    bool bv = (b[i / 64] >> (i % 64)) & 1;
                    if (av)
                        positions.push_back(i);
                    if (i < limit) {
                        intersection += av && bv;
                        difference += av && !bv;
                        cardinality += av;
                    }
                }
                require(bit_ops::count(a.data(), n, limit) == cardinality, "Raw count");
                require(bit_ops::intersection_count(a.data(), b.data(), n, limit) == intersection,
                        "Raw intersection count");
                require(bit_ops::difference_count(a.data(), b.data(), n, limit) == difference,
                        "Raw difference count");
                auto check_output = [&](const auto &words, bool subtract) {
                    for (size_t i = 0; i < nw * 64; ++i) {
                        const bool actual = (words[i / 64] >> (i % 64)) & 1;
                        const bool av = (a[i / 64] >> (i % 64)) & 1;
                        const bool bv = (b[i / 64] >> (i % 64)) & 1;
                        require(actual == (i < n && i < limit && av && (subtract ? !bv : bv)),
                                "Raw output/tail bits");
                    }
                };
                require(bit_ops::intersection_write(a.data(), b.data(), n, output.data(), limit) ==
                            intersection,
                        "Intersection write count");
                check_output(output, false);
                require(bit_ops::difference_write(a.data(), b.data(), n, output.data(), limit) ==
                            difference,
                        "Difference write count");
                check_output(output, true);
                auto alias = a;
                bit_ops::difference_write(alias.data(), b.data(), n, alias.data(), limit);
                check_output(alias, true);
                alias = b;
                bit_ops::intersection_write(a.data(), alias.data(), n, alias.data(), limit);
                check_output(alias, false);
                alias = a;
                bit_ops::copy_prefix(alias.data(), n, alias.data(), limit);
                require(bit_ops::count(alias.data(), n) == cardinality, "In-place bound");
                std::vector<size_t> scanned;
                bit_ops::for_each(a.data(), n, [&](size_t pos) { scanned.push_back(pos); });
                require(scanned == positions, "Bit iteration");
            }
        }
    }
    require(bit_ops::trailing_zeros(0) == 64 && bit_ops::popcount(~uint64_t{0}) == 64,
            "Word primitives");
    require(bit_ops::intersection_count(nullptr, nullptr, 0) == 0, "Null empty bitmap");
}

void containers(std::mt19937_64 &rng) {
    for (size_t n : {0, 1, 63, 64, 65, 127, 128, 129, 300}) {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < n; ++i)
            ids.push_back(static_cast<uint32_t>(i * 3 + 1));
        if (n)
            ids.back() = UINT32_MAX;
        NeighborhoodUniverse universe(42, ids);
        Bitmap full(universe, true);
        require(full.view().vertices() == ids, "Full universe/unsigned IDs");
        for (int trial = 0; trial < 40; ++trial) {
            std::vector<uint32_t> a, b;
            for (auto id : ids) {
                if (rng() % 2)
                    a.push_back(id);
                if (rng() % 2)
                    b.push_back(id);
            }
            auto aa = Bitmap::from_sorted(universe, a.data(), a.size());
            auto bb = Bitmap::from_sorted(universe, b.data(), b.size());
            require(aa.view().vertices() == a, "Round trip");
            for (auto upper : {std::optional<uint32_t>{}, std::optional<uint32_t>{0},
                               std::optional<uint32_t>{64}, std::optional<uint32_t>{UINT32_MAX}}) {
                for (uint32_t owner : {0u, 1u, 64u, UINT32_MAX}) {
                    std::vector<uint32_t> expected_i, expected_d, expected_s;
                    for (auto v : a) {
                        if (upper && v >= *upper)
                            continue;
                        const bool found = std::binary_search(b.begin(), b.end(), v);
                        if (found)
                            expected_i.push_back(v);
                        else {
                            expected_d.push_back(v);
                            if (v != owner)
                                expected_s.push_back(v);
                        }
                    }
                    auto i = aa.view().intersect(bb.view(), upper);
                    auto d = aa.view().difference(bb.view(), upper);
                    auto s = aa.view().subtract(bb.view(), owner, upper);
                    require(i.view().vertices() == expected_i &&
                                aa.view().intersect_count(bb.view(), upper) == expected_i.size(),
                            "Bitmap intersection");
                    require(d.view().vertices() == expected_d &&
                                aa.view().difference_count(bb.view(), upper) == expected_d.size(),
                            "Pure difference");
                    require(s.view().vertices() == expected_s &&
                                aa.view().subtract_count(bb.view(), owner, upper) == expected_s.size(),
                            "Owner exclusion");
                    const auto array_count =
                        upper ? set_ops::difference_bounded<false>(a.data(), a.size(), b.data(),
                                                                   b.size(), owner, *upper)
                              : set_ops::difference_count(a.data(), a.size(), b.data(), b.size(), owner);
                    require(array_count == expected_s.size(), "Array/bitmap parity");
                }
                if (upper) {
                    auto bounded = aa.view().bounded(*upper);
                    std::vector<uint32_t> expected;
                    for (auto id : a)
                        if (id < *upper)
                            expected.push_back(id);
                    require(bounded.view().vertices() == expected &&
                                aa.view().count(upper) == expected.size(),
                            "Global bound mapping");
                }
            }
            auto copy = aa;
            for (auto id : a)
                copy.clear(id);
            require(copy.view().count() == 0 && aa.view().vertices() == a, "Deep copy");
            auto moved = std::move(copy);
            require(moved.view().count() == 0, "Move transfer");
            rejects([&] { (void)copy.view(); });
        }
        NeighborhoodUniverse different(42, ids);
        Bitmap other(different, true);
        rejects([&] { (void)full.view().intersect_count(other.view()); });
        rejects([&] { (void)full.view().difference(other.view()); });
    }
    rejects([] { NeighborhoodUniverse bad(0, std::vector<uint32_t>{1, 1}); });
    rejects([] { NeighborhoodUniverse bad(0, std::vector<uint32_t>{2, 1}); });
    rejects([] { NeighborhoodUniverse bad(0, nullptr, 1); });
    rejects([] { NeighborhoodUniverse bad(0, nullptr, size_t{UINT32_MAX} + 1); });
    NeighborhoodUniverse u(0, std::vector<uint32_t>{1, 3, UINT32_MAX});
    rejects([&] { BitmapView bad(u, nullptr, 1); });
    rejects([&] { BitmapView bad(u, nullptr, 0); });
    uint32_t outside[] = {0, 1, 2, UINT32_MAX};
    rejects([&] { (void)Bitmap::from_sorted(u, outside, 4); });
    auto restricted = Bitmap::from_neighbors(u, outside, 4);
    require(restricted.view().vertices() == std::vector<uint32_t>({1, UINT32_MAX}),
            "Explicit adjacency restriction");
    uint64_t dirty = ~uint64_t{0};
    BitmapView view(u, &dirty, 1);
    require(view.count() == 3, "Borrowed dirty tail");
    auto retained = [] {
        NeighborhoodUniverse temporary(9, std::vector<uint32_t>{2, 4});
        return Bitmap(temporary, true);
    }();
    require(retained.view().vertices() == std::vector<uint32_t>({2, 4}), "Mapping lifetime");
}

void graphs(std::mt19937_64 &rng) {
    for (size_t n : {0, 1, 4, 65, 96}) {
        for (int density : {0, 25, 100}) {
            std::vector<std::vector<uint32_t>> adjacency(n);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                    if (rng() % 100 < static_cast<unsigned>(density)) {
                        adjacency[i].push_back(j);
                        adjacency[j].push_back(i);
                    }
            uint64_t triangles = 0, paths = 0, expected_triangles = 0, expected_paths = 0;
            auto linked = [&](size_t i, size_t j) {
                return std::binary_search(adjacency[i].begin(), adjacency[i].end(), j);
            };
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                    for (size_t k = j + 1; k < n; ++k) {
                        const int edges = linked(i, j) + linked(i, k) + linked(j, k);
                        expected_triangles += edges == 3;
                        expected_paths += edges == 2;
                    }
            for (size_t u = 0; u < n; ++u) {
                NeighborhoodUniverse universe(u, adjacency[u]);
                BitGraph graph(universe, adjacency[u],
                               [&](uint32_t v) -> const auto & { return adjacency[v]; });
                Bitmap full(universe, true);
                auto below_u = full.view().bounded(u);
                below_u.view().for_each(
                    [&](uint32_t v) { triangles += below_u.view().intersect_count(graph.row(v), v); });
                full.view().for_each(
                    [&](uint32_t v) { paths += full.view().subtract_count(graph.row(v), v, v); });
                for (auto v : adjacency[u]) {
                    std::vector<uint32_t> expected;
                    for (auto w : adjacency[v])
                        if (linked(u, w))
                            expected.push_back(w);
                    require(graph.row(v).vertices() == expected, "BitGraph restricted row");
                }
                rejects([&] { (void)graph.row(UINT32_MAX); });
                rejects([&] { (void)graph.row_at(graph.row_count()); });
            }
            require(triangles == expected_triangles && paths == expected_paths,
                    "Symmetry-free graph oracle");
        }
    }
    NeighborhoodUniverse u(0, std::vector<uint32_t>{1, 3});
    BitGraph outside_rows(u, {99}, [](uint32_t) { return std::vector<uint32_t>{0, 1, 3, 100}; });
    rejects([&] { BitGraph bad(u, {99, 99}, [](uint32_t) { return std::vector<uint32_t>{}; }); });
    require(outside_rows.row(99).vertices() == u.ids(), "Row IDs need not equal column universe");
    NeighborhoodUniverse empty(0, nullptr, 0);
    BitGraph empty_columns(empty, {1}, [](uint32_t) { return std::vector<uint32_t>{2}; });
    require(empty_columns.row(1).count() == 0 && empty_columns.storage_bytes() == 0, "Empty columns");
}
} // namespace
int main() {
    std::mt19937_64 rng(20260907);
    raw_kernels(rng);
    containers(rng);
    graphs(rng);
    std::cout << "Validated bitmap kernels, universe/ownership contracts, array parity, and BitGraph "
                 "graph oracles\n";
}
