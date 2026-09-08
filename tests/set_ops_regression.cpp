#ifdef GRAPHMINI_PROFILE_RUNTIME
#include "backend_prof/vertex_set.h"
#else
#include "backend/vertex_set.h"
#endif
#include <algorithm>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

using namespace minigraph;
using Kernel = size_t (*)(const uint32_t*, size_t, const uint32_t*, size_t, uint32_t*);
void require(bool b) { if (!b) throw std::runtime_error("set operation mismatch"); }

void check(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> expected;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(expected));
    // Exact input allocations with deliberately unaligned starts. ASan catches
    // vector loads beyond either input and stores past the exact output size.
    std::unique_ptr<uint32_t[]> aa(new uint32_t[a.size() + 1]), bb(new uint32_t[b.size() + 1]);
    std::copy(a.begin(), a.end(), aa.get() + 1);
    std::copy(b.begin(), b.end(), bb.get() + 1);
    const auto ap = a.empty() ? nullptr : aa.get() + 1;
    const auto bp = b.empty() ? nullptr : bb.get() + 1;
    std::vector<Kernel> kernels{[](const uint32_t* a, size_t na, const uint32_t* b, size_t nb, uint32_t* out) { return set_ops::scalar<true>(a, na, b, nb, out); }, set_ops::intersection<true>};
#if defined(__aarch64__)
    kernels.push_back(set_ops::neon<true>);
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (set_ops::has_avx2()) kernels.push_back(set_ops::avx2<true>);
#endif
    for (auto kernel : kernels) {
        std::unique_ptr<uint32_t[]> out(expected.empty() ? nullptr : new uint32_t[expected.size()]);
        const size_t n = kernel(ap, a.size(), bp, b.size(), out.get());
        require(n == expected.size());
        for (size_t i = 0; i < n; ++i) require(out[i] == expected[i]);
    }
    require(set_ops::intersection_count(ap, a.size(), bp, b.size()) == expected.size());
#if defined(__aarch64__)
    require(set_ops::neon<false>(ap, a.size(), bp, b.size()) == expected.size());
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (set_ops::has_avx2())
        require(set_ops::avx2<false>(ap, a.size(), bp, b.size()) == expected.size());
#endif
    VertexSet va(0, ap, a.size()), vb(1, bp, b.size());
    require(va.intersect_cnt(vb) == expected.size());
    auto result = va.intersect(vb);
    require(result.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) require(result[i] == expected[i]);
    for (uint32_t upper : {0u, 1u, 31u, 64u, 0x80000000u, 0xffffffffu}) {
        const size_t n = std::lower_bound(expected.begin(), expected.end(), upper) - expected.begin();
        require(va.intersect_cnt(vb, upper) == n);
        auto bounded = va.intersect(vb, upper);
        require(bounded.size() == n);
        for (size_t i = 0; i < n; ++i) require(bounded[i] == expected[i]);
        std::unique_ptr<uint32_t[]> out(n ? new uint32_t[n] : nullptr);
        require(va.intersect(vb, upper, out.get()) == n);
        for (size_t i = 0; i < n; ++i) require(out[i] == expected[i]);
    }
}
int main() {
    VertexSet::MAX_DEGREE = 4096;
#ifdef GRAPHMINI_PROFILE_RUNTIME
    VertexSet::profiler = std::make_shared<Profiler>(2, 2);
#endif
    size_t cases = 0;
    // Exhaust all pairs of subsets of a small universe.
    for (unsigned x = 0; x < 64; ++x) for (unsigned y = 0; y < 64; ++y) {
        std::vector<uint32_t> a, b;
        for (unsigned k = 0; k < 6; ++k) {
            if (x & (1u << k)) a.push_back(k);
            if (y & (1u << k)) b.push_back(k);
        }
        check(a, b); ++cases;
    }
    std::mt19937 rng(20260907);
    for (size_t na : {0,1,3,4,7,8,9,15,16,31,32,33,63,64,65,128,513})
        for (size_t nb : {0,1,3,4,7,8,9,15,16,31,32,33,63,64,65,128,513})
            for (unsigned kind = 0; kind < 5; ++kind) {
                std::vector<uint32_t> a, b;
                for (size_t i = 0; i < na; ++i) a.push_back(kind == 0 ? rng() : i * 2u);
                for (size_t i = 0; i < nb; ++i)
                    b.push_back(kind == 0 ? rng() : kind == 1 ? i * 2u : kind == 2 ? i * 2u + 1 : i * 7u);
                if (kind == 4) {
                    for (auto& x : a) x += 0x80000000u;
                    for (auto& x : b) x += 0x80000000u;
                    a.push_back(0xffffffffu); b.push_back(0xffffffffu);
                }
                for (auto* v : {&a, &b}) {
                    std::sort(v->begin(), v->end());
                    v->erase(std::unique(v->begin(), v->end()), v->end());
                }
                check(a,b); ++cases;
            }
    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<uint32_t> a, b;
        for (unsigned k = 0; k < 1024; ++k) {
            if (rng() % 3 == 0) a.push_back(k);
            if (rng() % 3 == 0) b.push_back(k);
        }
        check(a, b); ++cases;
    }
    std::cout << "Validated " << cases << " set pairs, direct SIMD and dispatch, bounded wrappers\n";
}
