#include "compiler/execution_ir.h"
#include "compiler/planning.h"
#include <iostream>
#include <stdexcept>

using namespace minigraph;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    size_t selected = 0;
    for (int n = 4; n <= 7; ++n) {
        for (int missing = 0; missing < 4; ++missing) {
            std::string query(n * n, '1');
            for (int i = 0; i < n; ++i)
                query[i * n + i] = '0';
            if (missing)
                query[1] = query[n] = '0';
            if (missing == 2)
                query[2] = query[2 * n] = '0';
            if (missing == 3)
                query[2 * n + 3] = query[3 * n + 2] = '0';
            for (auto scheduler :
                 {SchedulerType::GraphPi, SchedulerType::GraphMini, SchedulerType::GraphZero}) {
                CodeGenConfig config;
                config.pruningType = PruningType::None;
                config.parType = ParallelType::OpenMP;
                config.schedulerType = scheduler;
                config.bitmap = true;
                const auto plan = compile_vertex_induced(query, config, meta);
                const auto ir = lower_execution(plan);
                require(dump_execution(ir) == dump_execution(lower_execution(plan)),
                        "Nondeterministic bitmap plan");
                if (ir.bitmap_region) {
                    ++selected;
                    for (int mutation = 0; mutation < 6; ++mutation) {
                        auto bad = ir;
                        auto &r = *bad.bitmap_region;
                        if (mutation == 0)
                            ++r.entry_depth;
                        if (mutation == 1)
                            r.anchor_depth = n;
                        if (mutation == 2)
                            r.conversion_depth = -1;
                        if (mutation == 3)
                            r.live_ins.clear();
                        if (mutation == 4)
                            r.count_ops.clear();
                        if (mutation == 5)
                            r.iterator_set = -1;
                        bool rejected = false;
                        try {
                            verify_execution(bad, plan);
                        } catch (const std::logic_error &) {
                            rejected = true;
                        }
                        require(rejected, "Accepted malformed bitmap region");
                    }
                } else
                    require(!ir.bitmap_reason.empty(), "Missing fallback explanation");
                auto arrays = plan;
                arrays.context.config.bitmap = false;
                require(!lower_execution(arrays).bitmap_region, "Default selected bitmap");
                auto unsupported = plan;
                unsupported.context.config.parType = ParallelType::NestedRt;
                require(!lower_execution(unsupported).bitmap_region,
                        "Unsupported task capture accepted");
                if (n == 4 && missing == 0 && scheduler == SchedulerType::GraphPi)
                    require(gen_code(query, unsupported.context.config, meta).find("// bitmap: requires") == 0,
                            "Missing generated-code fallback explanation");
            }
        }
    }
    require(selected > 0, "No bitmap plans exercised");
    std::cout << "Validated 48 bitmap planning cases; selected " << selected << " regions\n";
}
