// Correctness and optional microbenchmark for production fixed-word kernels.
#include "backend/bit_ops/bit_ops.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace minigraph::bit_ops;
template<size_t Words, Binary Op, bool Write>
inline size_t fixed(const Word *a, const Word *b, size_t bits, Word *out, size_t limit) {
    return combine_fixed<Words, Op, Write>(a, b, bits, out, limit);
}
struct Item { std::array<Word, 2> a, b; size_t bound; };
template<Binary Op, bool Write> void verify(std::mt19937_64 &rng) {
    for (size_t bits = 1; bits <= 128; ++bits)
        for (size_t limit = 0; limit <= 130; ++limit) {
            std::vector<Word> a(word_count(bits)), b(a.size()), out(a.size(), ~Word{0});
            for (auto &v : a) v = rng();
            for (auto &v : b) v = rng();
            std::vector<Word> expected(a.size());
            size_t count = 0;
            for (size_t i = 0; i < std::min(bits, limit); ++i) {
                bool value = ((a[i/64] >> (i%64)) & 1) &&
                    (((b[i/64] >> (i%64)) & 1) == (Op == Binary::Intersection));
                if (value) { expected[i/64] |= Word{1} << (i%64); ++count; }
            }
            auto got = bits <= 64 ? fixed<1, Op, Write>(a.data(), b.data(), bits, out.data(), limit)
                                  : fixed<2, Op, Write>(a.data(), b.data(), bits, out.data(), limit);
            if (got != count || (Write && out != expected)) throw std::runtime_error("fixed mismatch");
            if (combine<Op, Write>(a.data(), b.data(), bits, out.data(), limit) != count ||
                (Write && out != expected)) throw std::runtime_error("baseline mismatch");
            if constexpr (Write) {
                if (bits <= 64) fixed<1, Op, true>(a.data(), b.data(), bits, a.data(), limit);
                else fixed<2, Op, true>(a.data(), b.data(), bits, a.data(), limit);
                if (a != expected) throw std::runtime_error("alias mismatch");
            }
        }
}
// One call per simulated region, not per operation. Runtime bits/limits remain
// unknown to the callee; Words is the only compile-time universe information.
template<int Mode, size_t Words, Binary Op, bool Write>
__attribute__((noinline)) Word batch(const std::vector<Item> &items, size_t bits, bool bounded) {
    Word sum = 0;
    std::array<Word, 2> out{};
    for (const auto &item : items) {
        const size_t limit = bounded ? item.bound : unlimited;
        if constexpr (Mode == 0) sum += combine<Op, Write>(item.a.data(), item.b.data(), bits, out.data(), limit);
        if constexpr (Mode == 1) sum += fixed<Words, Op, Write>(item.a.data(), item.b.data(), bits, out.data(), limit);
        if constexpr (Mode == 2) {
            sum += bits <= 64 ? fixed<1, Op, Write>(item.a.data(), item.b.data(), bits, out.data(), limit)
                              : fixed<2, Op, Write>(item.a.data(), item.b.data(), bits, out.data(), limit);
        }
        if constexpr (Write) {
            sum ^= out[0];
            if constexpr (Words == 2) sum += out[1];
        }
    }
    return sum;
}
template<size_t Words, Binary Op, bool Write>
void measure(const std::vector<Item> &items, size_t bits, bool bounded) {
    std::array<std::vector<double>, 3> times;
    volatile Word sink = 0;
    const auto expected = batch<0, Words, Op, Write>(items, bits, bounded);
    if (batch<1, Words, Op, Write>(items,bits,bounded) != expected ||
        batch<2, Words, Op, Write>(items,bits,bounded) != expected) throw std::runtime_error("batch mismatch");
    for (int trial = 0; trial < 9; ++trial)
        for (int order = 0; order < 3; ++order) {
            int mode = (trial + order) % 3;
            const auto start = std::chrono::steady_clock::now();
            for (int r = 0; r < 3000; ++r) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (mode == 0) sink ^= batch<0, Words, Op, Write>(items,bits,bounded);
                if (mode == 1) sink ^= batch<1, Words, Op, Write>(items,bits,bounded);
                if (mode == 2) sink ^= batch<2, Words, Op, Write>(items,bits,bounded);
            }
            times[mode].push_back(std::chrono::duration<double,std::nano>(
                std::chrono::steady_clock::now()-start).count()/(3000*items.size()));
        }
    for (auto &v : times) std::sort(v.begin(),v.end());
    std::cout << bits << ',' << bounded << ',' << (Op==Binary::Difference) << ',' << Write
              << ',' << times[0][4] << ',' << times[1][4] << ',' << times[2][4]
              << ',' << times[0][4]/times[1][4] << '\n';
}
int main(int argc, char **) {
    std::mt19937_64 rng(917);
    verify<Binary::Intersection,false>(rng); verify<Binary::Difference,false>(rng);
    verify<Binary::Intersection,true>(rng); verify<Binary::Difference,true>(rng);
    std::cerr << "Validated all sizes 1..128 and bounds 0..130 against bit oracle\n";
    if (argc == 1) return 0;
    std::vector<Item> items(256);
    for (auto &v : items) v = {{rng(),rng()},{rng(),rng()},rng()%129};
    std::cout << "bits,bounded,difference,write,dynamic_ns,fixed_ns,branch_ns,speedup\n";
    for (size_t bits : {32,64,96,128}) for (bool bound : {false,true}) {
#define RUN(OP,W) if(bits<=64) measure<1,OP,W>(items,bits,bound); else measure<2,OP,W>(items,bits,bound)
        RUN(Binary::Intersection,false); RUN(Binary::Difference,false);
        RUN(Binary::Intersection,true); RUN(Binary::Difference,true);
#undef RUN
    }
}
