#include "backend/bitmap_tasks.h"
#include "backend/bitmap_dispatch.h"
#include <oneapi/tbb/global_control.h>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
using namespace minigraph;
void require(bool ok) { if (!ok) throw std::runtime_error("Explicit bitmap regression"); }
int main() {
    tbb::global_control workers(tbb::global_control::max_allowed_parallelism, 4);
    std::mt19937 rng(914);
    for (size_t n : {0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1025}) {
        std::vector<uint32_t> ids(n);
        for (size_t i = 0; i < n; ++i) ids[i] = 3 * i + 11;
        if (n) ids.back() = UINT32_MAX;
        NeighborhoodUniverse universe(9, ids);
        Bitmap a(universe), b(universe), output(universe);
        std::vector<bool> av(n), bv(n);
        for (size_t i = 0; i < n; ++i) {
            if ((av[i] = rng() % 2)) a.set(ids[i]);
            if ((bv[i] = rng() % 2)) b.set(ids[i]);
        }
        const auto row_ids = b.view().vertices();
        BitGraph graph(universe, ids, [&](uint32_t) -> const std::vector<uint32_t>& { return row_ids; });
        auto check = [&](auto tag) {
            constexpr size_t Words = decltype(tag)::value;
            for (size_t upper : {size_t{0}, n/2, n, n+1}) {
                for (uint32_t excluded : {uint32_t{0}, static_cast<uint32_t>(n/2), static_cast<uint32_t>(n)}) {
                    std::vector<uint32_t> intersection, subtraction, bounded, removed;
                    for (size_t i = 0; i < n && i < upper; ++i) {
                        if (av[i] && bv[i]) intersection.push_back(ids[i]);
                        if (av[i] && !bv[i] && i != excluded) subtraction.push_back(ids[i]);
                        if (av[i]) bounded.push_back(ids[i]);
                        if (av[i] && i != excluded) removed.push_back(ids[i]);
                    }
                    output.assign_intersection<Words>(a, b.view(), upper);
                    require(output.view().vertices() == intersection && output.count() == intersection.size());
                    require(a.intersection_count<Words>(b.view(), upper) == intersection.size());
                    if (n) {
                        const auto row = graph.local_row(static_cast<uint32_t>(n-1));
                        require(row.data() == graph.row_data_at(n-1));
                        require(&row.universe() == &graph.universe());
                        require(a.intersection_count<Words>(row, upper) == intersection.size());
                        output.assign_subtraction<Words>(a, row, excluded, upper);
                        require(output.view().vertices() == subtraction);
                    }
                    output.assign_subtraction<Words>(a, b.view(), excluded, upper);
                    require(output.view().vertices() == subtraction && output.count() == subtraction.size());
                    require(a.subtraction_count<Words>(b.view(), excluded, upper) == subtraction.size());
                    output.assign_bounded<Words>(a, upper);
                    require(output.view().vertices() == bounded && a.bounded_count<Words>(upper) == bounded.size());
                    output.assign_removed<Words>(a, excluded, upper);
                    require(output.view().vertices() == removed && a.removed_count<Words>(excluded, upper) == removed.size());
                    auto alias = a;
                    alias.assign_subtraction<Words>(alias, b.view(), excluded, upper);
                    require(alias.view().vertices() == subtraction);
                    alias = a;
                    alias.assign_intersection<Words>(alias, alias.view(), upper);
                    require(alias.view().vertices() == bounded);
                    // Write-only variants preserve contents, carry only safe
                    // bounds, and keep existing borrowed views up to date.
                    const auto borrowed = output.view();
                    auto lazy_check = [&](const std::vector<uint32_t> &expected) {
                        require(output.count() == expected.size());
                        require(borrowed.count() == expected.size());
                        require(borrowed.vertices() == expected);
                        require(output.capacity_bound() >= expected.size());
                        require(output.capacity_bound() <= std::min(n, upper));
                        require(output.empty() == expected.empty());
                    };
                    output.assign_intersection<Words, false>(a, b.view(), upper);
                    lazy_check(intersection);
                    require(output.capacity_bound() <= b.count());
                    output.assign_subtraction<Words, false>(a, b.view(), excluded, upper);
                    lazy_check(subtraction);
                    output.assign_bounded<Words, false>(a, upper);
                    lazy_check(bounded);
                    output.assign_removed<Words, false>(a, excluded, upper);
                    lazy_check(removed);
                    alias = a;
                    alias.assign_bounded<Words, false>(alias, upper);
                    alias.assign_subtraction<Words, false>(alias, b.view(), excluded, upper);
                    require(alias.view().vertices() == subtraction && alias.capacity_bound() >= subtraction.size());
                    alias = a;
                    alias.assign_intersection<Words, false>(alias, alias.view(), upper);
                    require(alias.view().vertices() == bounded);
                    alias = b;
                    alias.assign_intersection<Words, false>(a, alias.view(), upper);
                    require(alias.view().vertices() == intersection);
                    alias = b;
                    alias.assign_subtraction<Words, false>(a, alias.view(), excluded, upper);
                    require(alias.view().vertices() == subtraction);
                    if (n) {
                        const auto row = graph.local_row(static_cast<uint32_t>(n-1));
                        require(row.count() == b.count() && graph.row_at(n-1).count() == b.count());
                        require(graph.row_cardinality_at(n-1) == b.count());
                        output.assign_intersection<Words, false>(a, row, upper);
                        lazy_check(intersection);
                        require(output.capacity_bound() <= row.count());
                    }
                }
            }
        };
        dispatch_bitmap_words(n, check);
        check(std::integral_constant<size_t, 0>{});
        for (const auto policy : {BitmapTaskPolicy{}, BitmapTaskPolicy{64, 99, true},
                                  BitmapTaskPolicy{64, 99, true, true}, BitmapTaskPolicy{64, 0, true},
                                  BitmapTaskPolicy{64, 99, true, false, BitmapIteration::DecodedScalar},
                                  BitmapTaskPolicy{64, 99, true, false, BitmapIteration::DecodedAVX2},
                                  BitmapTaskPolicy{64, 99, true, true, BitmapIteration::DecodedAVX2}}) {
            for (bool parallel : {false, true}) {
                const auto before = a.view().vertices();
                const auto actual = bitmap_for_each(a, parallel,
                    [&](auto cursor, bool task) -> uint64_t {
                        BitmapTaskInput input(a, task, policy);
                        const auto &s0 = input.get();
                        const bool copies = task && (n <= 512 || policy.copy_inputs);
                        if (n) require((s0.words().data() != a.words().data()) == copies);
                        Bitmap s1(universe);
                        uint64_t total = 0;
                        for (; cursor.valid(); cursor.advance()) {
                            s1.assign_bounded<0, false>(s0, cursor.position());
                            // A private output is reused only after this nested join.
                            total += bitmap_for_each(s1, parallel,
                                [&](auto c, bool child_task) -> uint64_t {
                                    BitmapTaskInput child(s1, child_task, policy);
                                    uint64_t count = 0;
                                    for (; c.valid(); c.advance()) ++count;
                                    return count;
                                }, policy, 1);
                        }
                        return total;
                    }, policy);
                const auto k = a.count();
                require(actual == (k ? k*(k-1)/2 : 0));
                require(a.view().vertices() == before);
            }
        }
    }
    NeighborhoodUniverse left(1, {1, 2}), right(1, {1, 3});
    Bitmap a(left), b(right), out(left);
    require(bitmap_for_each(a, true, [](auto, bool) -> uint64_t {
        throw std::runtime_error("Empty input constructed task scratch");
    }) == 0);
    bool rejected = false;
    try { out.assign_intersection(a, b.view()); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected);
    BitGraph partial(left, {1}, [](uint32_t) { return std::vector<uint32_t>{2}; });
    rejected = false;
    try { partial.local_row(0); } catch (const std::logic_error &) { rejected = true; }
    require(rejected);
}
