#include "runtime/graph_builder.h"
#include "bitmap_count_only_fixture.cpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace minigraph;

uint64_t choose(unsigned n, unsigned k) {
    if (n < k) return 0;
    uint64_t result = 1;
    for (unsigned i = 1; i <= k; ++i) result = result * (n - k + i) / i;
    return result;
}

int checks = 0;
void check(std::vector<std::vector<IdType>> rows, uint64_t expected, int bitmap = -1) {
    GraphCSRData csr;
    csr.indptr.push_back(0);
    for (auto& row : rows) {
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
        csr.indices.insert(csr.indices.end(), row.begin(), row.end());
        csr.indptr.push_back(csr.indices.size());
    }
    auto graph = build_graph_from_csr(std::move(csr));
    for (int threads : {1, 4}) {
        Context ctx(256);
        ctx.num_threads = threads;
        minigraph::plan(graph.get(), ctx);
        if (static_cast<uint64_t>(ctx.get_result()) != expected)
            throw std::runtime_error("Count-only fixture count mismatch");
        if (bitmap >= 0 && graphmini_bitmap_counter(bitmap ? 3 : 5) == 0)
            throw std::runtime_error("Expected count-only execution path was not exercised");
        ++checks;
    }
}

int main() {
    for (unsigned n : {4, 6, 8, 9})
        for (bool missing : {false, true}) {
            std::vector<std::vector<IdType>> rows(n);
            for (unsigned i = 0; i < n; ++i)
                for (unsigned j = 0; j < n; ++j)
                    if (i != j && !(missing && i + j == 1)) rows[i].push_back(j);
            check(std::move(rows), choose(n, 6) - (missing ? choose(n - 2, 4) : 0));
        }
    // One six-clique at the high-ID end of a star: expected count is one at
    // each word boundary, including a universe that exceeds the row budget.
    for (unsigned degree : {63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 20000}) {
        std::vector<std::vector<IdType>> rows(degree + 1);
        for (unsigned i = degree - 5; i <= degree; ++i)
            for (unsigned j = degree - 5; j <= degree; ++j)
                if (i != j) rows[i].push_back(j);
        for (unsigned i = 0; i < degree - 5; ++i) {
            rows[i].push_back(degree);
            rows[degree].push_back(i);
        }
        check(std::move(rows), 1, degree == 20000 ? 0 : 1);
    }
    std::cout << "Passed " << checks << " count-only native checks\n";
}
