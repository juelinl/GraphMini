#include "backend/iep_bitmap.h"
#include <iostream>
#include <limits>

using namespace minigraph;
void require(bool value) { if (!value) throw std::runtime_error("IEP bitmap regression"); }
int main() {
    for (size_t n : {0, 1, 63, 64, 65, 127, 128, 129, 257}) {
        std::vector<IdType> u, a, b, c, d;
        size_t two = 0, three = 0, four = 0;
        for (size_t i = 0; i < n; ++i) {
            IdType v = i + 1 == n ? std::numeric_limits<IdType>::max() : i * 7;
            u.push_back(v);
            if (i % 2 == 0) a.push_back(v);
            if (i % 3 != 0) b.push_back(v);
            if (i % 5 != 0) c.push_back(v);
            if (i % 7 != 0) d.push_back(v);
            two += i % 2 == 0 && i % 3 != 0;
            three += i % 2 == 0 && i % 3 != 0 && i % 5 != 0;
            four += i % 2 == 0 && i % 3 != 0 && i % 5 != 0 && i % 7 != 0;
        }
        VertexSet universe(0, u.data(), u.size()), sa(0, a.data(), a.size()),
            sb(0, b.data(), b.size()), sc(0, c.data(), c.size()), sd(0, d.data(), d.size());
        IEPBitmap bitmap(universe, {&sa, &sb, &sc, &sd});
        require(bitmap.enabled());
        require(bitmap.intersection_count({0, 1}) == two);
        require(bitmap.intersection_count({0, 1, 2}) == three);
        require(bitmap.intersection_count({0, 1, 2, 3}) == four);
        require(bitmap.intersection_count({0, 1}) == two);
        IEPBitmap disabled(universe, {&sa}, 0);
        require(disabled.enabled() == (n == 0));
        bool rejected = false;
        try { bitmap.intersection_count({0, 4}); } catch (const std::out_of_range &) { rejected = true; }
        require(rejected);
    }
    IdType u[] = {1, 3}, a[] = {2};
    VertexSet universe(0, u, 2), candidate(0, a, 1);
    bool rejected = false;
    try { IEPBitmap invalid(universe, {&candidate}); } catch (const std::logic_error &) { rejected = true; }
    require(rejected);
    std::cout << "IEP bitmap boundary and factor checks passed\n";
}
