#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <iostream>
#include <stdexcept>
using namespace minigraph;
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    int selected = 0;
    for (int n : {6, 7, 8}) {
        const int core = n - 3;
        std::string query(n*n, '0');
        auto edge = [&](int a, int b) { query[a*n+b] = query[b*n+a] = '1'; };
        for (int a = 0; a < core; ++a)
            for (int b = a+1; b < core; ++b) edge(a,b);
        for (int leaf = 0; leaf < 3; ++leaf)
            for (int a = 0; a < core; ++a)
                if (a == 0 || a != leaf+1) edge(a,core+leaf);
        CodeGenConfig config;
        config.schedulerType = SchedulerType::Outgoing;
        config.pruningType = PruningType::None;
        config.bitmap = true;
        auto plan = compile_edge_induced_iep(query, config, meta);
        auto ir = lower_execution(plan);
        std::cout << "n=" << n << " width=" << plan.counting.iep_num << '\n' << dump_execution(ir);
        if (ir.iep_bitmap || ir.bitmap_region)
            throw std::runtime_error("IEP must use arrays");
        ++selected;
    }
    std::cout << "selected=" << selected << '\n';
    if (selected != 3) throw std::runtime_error("Incomplete bitmap coverage");
}
