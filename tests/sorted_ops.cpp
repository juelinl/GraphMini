#ifdef GRAPHMINI_PROFILE_RUNTIME
#include "backend_prof/minigraph.h"
#else
#include "backend/minigraph.h"
#endif
#include <random>
#include <iostream>

using namespace minigraph;
void require(bool ok) { if (!ok) throw std::runtime_error("Sorted operations regression"); }
void check(std::vector<uint32_t> a, std::vector<uint32_t> b) {
    auto clean = [](auto& x) { std::sort(x.begin(), x.end()); x.erase(std::unique(x.begin(), x.end()), x.end()); };
    clean(a); clean(b);
    const size_t na = a.size(), nb = b.size();
    // Deliberately unaligned starts and exact input lengths (no SIMD padding).
    auto aa = std::make_unique<uint32_t[]>(na + 1), bb = std::make_unique<uint32_t[]>(nb + 1);
    std::copy(a.begin(), a.end(), aa.get() + 1); std::copy(b.begin(), b.end(), bb.get() + 1);
    auto* ap = na ? aa.get() + 1 : nullptr; auto* bp = nb ? bb.get() + 1 : nullptr;
    std::vector<uint32_t> expected;
    for (size_t i = 0; i < na; ++i) if (std::binary_search(b.begin(), b.end(), a[i])) expected.push_back(i);
    auto output = expected.empty() ? nullptr : std::make_unique<uint32_t[]>(expected.size());
    auto verify = [&](size_t n) {
        require(n == expected.size());
        for (size_t i = 0; i < n; ++i) require(output[i] == expected[i]);
    };
    verify(set_ops::indices_scalar(ap, na, bp, nb, output.get()));
    verify(set_ops::indices_write(ap, na, bp, nb, output.get()));
    if (set_ops::sorted_simd_available()) verify(set_ops::indices_simd(ap, na, bp, nb, output.get()));
    VertexSet va(0, ap, na), vb(1, bp, nb);
    auto positions = va.indices(vb);
    auto managed = get_indices(va, vb);
    require(positions.size() == expected.size() && managed.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) require(positions[i] == expected[i] && managed[i] == expected[i]);
    std::vector<uint32_t> targets{0, 1, 0x7fffffff, 0x80000000, UINT32_MAX};
    if (na) { targets.push_back(a.front()); targets.push_back(a[na / 2]); targets.push_back(a.back()); }
    for (auto value : targets) {
        const size_t idx = std::lower_bound(a.begin(), a.end(), value) - a.begin();
        require(set_ops::advance_to(ap, na ? ap + na : ap, value) == (na ? ap + idx : ap));
        require(set_ops::lower_bound_index(ap, na, value) == idx);
        require(set_ops::lower_bound_binary(ap, na, value) == idx);
        if (set_ops::sorted_simd_available()) require(set_ops::lower_bound_simd(ap, na, value) == idx);
        auto bounded = va.bounded(value);
        require(bounded.size() == idx && !bounded.pooled() && va.bounded_cnt(value) == idx);
        const size_t count = na - (idx < na && a[idx] == value);
        require(va.remove_cnt(value) == count);
        auto removed = va.remove(value);
        require(removed.size() == count);
        size_t j = 0;
        for (auto id : a) if (id != value) require(removed[j++] == id);
        if (count != na) {
            auto exact = count ? std::make_unique<uint32_t[]>(count) : nullptr;
            set_ops::remove_at(ap, na, idx, exact.get());
            for (size_t k = 0; k < count; ++k) require(exact[k] == removed[k]);
        }
        VertexSet owner(na); owner.set_size(na);
        if (na) std::copy(a.begin(), a.end(), owner.begin());
        auto owned_view = std::move(owner).bounded(value);
        require(owned_view.pooled() && owned_view.size() == idx);
        auto retained = std::move(owned_view).remove(value); // absent at strict bound
        require(retained.pooled() && retained.size() == idx);
    }
    auto identity = va.indices(va);
    require(identity.size() == na);
    for (size_t i = 0; i < na; ++i) require(identity[i] == i);
}
int main() {
    size_t cases = 0;
    for (unsigned a = 0; a < 64; ++a) for (unsigned b = 0; b < 64; ++b) {
        std::vector<uint32_t> x, y;
        for (unsigned i = 0; i < 6; ++i) { if (a & (1u << i)) x.push_back(i); if (b & (1u << i)) y.push_back(i); }
        check(x, y); ++cases;
    }
    std::mt19937 rng(20260907);
    for (size_t n : {1, 3, 4, 7, 8, 9, 31, 32, 33, 63, 64, 65, 128, 1024}) {
        for (size_t m : {0, 1, 4, 8, 33, 64, 1024}) for (int trial = 0; trial < 8; ++trial) {
            std::vector<uint32_t> a(n), b(m);
            for (auto& x : a) x = trial % 2 ? rng() : rng() % 128;
            for (auto& x : b) x = trial % 2 ? rng() : rng() % 128;
            check(a, b); ++cases;
        }
    }
    // Left SIMD block matching several right blocks and a scalar tail.
    check({0, 3, 7, 1000, 2000, 3000, 4000, UINT32_MAX}, {0, 1, 2, 3, 4, 5, 6, 7, UINT32_MAX});
    std::vector<uint32_t> high(129);
    for (size_t i = 0; i < high.size(); ++i) high[i] = UINT32_MAX - 128 + i;
    check(high, high);
    std::cout << "Validated " << cases + 2 << " sorted-operation pairs and wrapper ownership\n";
}
