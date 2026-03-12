#include "codegen_output/plan_profile.h"
#include "configure.h"
#include "common.h"
#include "graph_loader.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <math.h>
#include <condition_variable>
#include <oneapi/tbb/global_control.h>
using namespace std::chrono_literals;
namespace minigraph {
    std::ostream &operator<<(std::ostream &os, const VertexSetType &dt) {
        if (dt.vid() == Constant::EmptyID<IdType>()) {
            os << "VertexSet(-1)\t=\t[";
        } else {
            os << "VertexSet(" << dt.vid() << ")\t=\t[";
        }
        constexpr uint64_t expected_to_show = 10;
        uint64_t actual_show = std::min(expected_to_show, dt.size());
        for (size_t i = 0; i < actual_show; i++) {
            os << dt[i] << ",";
        }
        if (expected_to_show < dt.size()) os << "...";
        os << "]\t";
        os << "(size=" << dt.size() << " pooled=" << dt.pooled() << ")\n";
        return os;
    }
}

namespace {
int default_thread_count() {
    const unsigned int detected = std::thread::hardware_concurrency();
    return std::max(1u, detected);
}

int parse_num_threads_or_default(int argc, char *argv[], int index) {
    if (argc <= index) {
        return default_thread_count();
    }
    const int parsed = std::stoi(argv[index]);
    return std::max(1, parsed);
}

bool parse_bool_or_default(int argc, char *argv[], int index, bool default_value) {
    if (argc <= index) {
        return default_value;
    }
    std::string value = argv[index];
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    throw std::invalid_argument(std::string("Invalid boolean flag: ") + argv[index]);
}
} // namespace

void plan_2hrs(minigraph::GraphType* graph, minigraph::Context& ctx) {
    std::mutex m;
    std::condition_variable cv;
    std::thread t([&cv, &graph, &ctx](){
        minigraph::plan(graph, ctx);
        cv.notify_one();
    });
    t.detach();
    {
        std::unique_lock<std::mutex> l(m);
        // timeout after 1 day = 24 * 3600s = 86400s
        // timeout after 2 hours = 2 * 3600s = 7200s
        if (cv.wait_for(l, 7200s) == std::cv_status::timeout) throw std::runtime_error("Timeout");
    }
};

void plan_24hrs(minigraph::GraphType* graph, minigraph::Context& ctx) {
    std::mutex m;
    std::condition_variable cv;
    std::thread t([&cv, &graph, &ctx](){
        minigraph::plan(graph, ctx);
        cv.notify_one();
    });
    t.detach();
    {
        std::unique_lock<std::mutex> l(m);
        // timeout after 1 day = 24 * 3600s = 86400s
        // timeout after 2 hours = 2 * 3600s = 7200s
        if (cv.wait_for(l, 86400s) == std::cv_status::timeout) throw std::runtime_error("Timeout");
    }
};

int main(int argc, char *argv[]){
    using namespace minigraph;
    int expId = std::stoi(argv[1]);
    std::string in_dir{argv[2]};
    std::string prof_aggr_path{argv[3]};
    std::string prof_loop_path{argv[4]};
    Timer t;
    const int processor_count = parse_num_threads_or_default(argc, argv, 5);
    const bool reorder_by_degree = parse_bool_or_default(argc, argv, 6, false);
    LOG(MSG) << "Thread Count: " << processor_count;
    double reindex_time = 0.0;
    GraphLoadOptions load_options;
    load_options.mmap = false;
    load_options.reorder_by_degree = reorder_by_degree;
    load_options.reindex_time_seconds = &reindex_time;
    GraphType *graph = load_bin<GraphType>(in_dir, load_options);
    if (reorder_by_degree) {
        LOG(MSG) << "Reindex Time: " << ToReadableDuration(reindex_time);
    } else {
        LOG(MSG) << "Reindex Time: disabled";
    }
    LOG(MSG) << "Load Time: " << ToReadableDuration(t.Passed());
    bool time_out = false;
    double seconds = 24 * 3600;
    t.Reset();
    tbb::global_control tbb_thread_limit(tbb::global_control::max_allowed_parallelism, processor_count);
    Context ctx(processor_count);
    ctx.profiler = std::make_shared<Profiler>(pattern_size(), graph->get_vnum());
    try {
        plan_24hrs(graph, ctx);
    } catch (std::runtime_error& e) {
        time_out = true;
    }

    RunnerLog log;
    long long result{0};
    if (time_out) {
        result = ctx.get_result();
        seconds = t.Passed();

        log.finished = false;
        log.expId = expId;
        log.result = result;
        log.numThread = processor_count;
        log.vertexAllocated = VertexSetType::TOTAL_ALLOCATED;
        log.miniGraphAllocated = VertexSetType ::TOTAL_ALLOCATED;
        log.runTime = seconds;
        log.threadMinTime = seconds;
        log.threadMeanTime = seconds;
        log.threadMaxTime = seconds;
        log.threadTimeSTD = 0.0;

        LOG(MSG) << "Execution Time: Timeout";
        LOG(MSG) << "Result: " << ctx.get_result();
        LOG(MSG) << "Throughput: " << result / seconds;
        LOG(MSG) << "Vertex Set Allocated: " << ToReadableSize(VertexSetType::TOTAL_ALLOCATED);
        LOG(MSG) << "MiniGraph Allocated: " << ToReadableSize(MiniGraphPool::TOTAL_ALLOCATED);
    } else {
        result = ctx.get_result();
        seconds = t.Passed();

        log.finished = true;
        log.expId = expId;
        log.result = result;
        log.numThread = processor_count;
        log.vertexAllocated = VertexSetType::TOTAL_ALLOCATED;
        log.miniGraphAllocated = MiniGraphPool::TOTAL_ALLOCATED;
        log.runTime = seconds;
        log.threadMinTime = ctx.get_min_time();
        log.threadMeanTime = ctx.get_mean_time();
        log.threadMaxTime = ctx.get_max_time();
        log.threadTimeSTD = sqrt(ctx.get_var_time());

        LOG(MSG) << "Execution Time: " << ToReadableDuration(seconds);
        LOG(MSG) << "Result: " << ctx.get_result();
        LOG(MSG) << "Set Comp: " << ctx.profiler->total_set_comp();
        LOG(MSG) << "MiniGraph Comp: " << ctx.profiler->total_mg_comp();
        LOG(MSG) << "Neighbor Comp: " << ctx.profiler->total_neb_comp();
        LOG(MSG) << "Total Comp: " << ctx.profiler->total_set_comp() + ctx.profiler->total_mg_comp();
        LOG(MSG) << "Throughput: " << ctx.get_result() / seconds;
        LOG(MSG) << "Thread Mean Time: " << ToReadableDuration(ctx.get_mean_time());
        LOG(MSG) << "Thread Min Time: " << ToReadableDuration(ctx.get_min_time());
        LOG(MSG) << "Thread Max Time: " << ToReadableDuration(ctx.get_max_time());
        LOG(MSG) << "Thread Time Std Dev: " << ToReadableDuration(sqrt(ctx.get_var_time()));
        LOG(MSG) << "Vertex Set Allocated: " << ToReadableSize(VertexSetType::TOTAL_ALLOCATED);
        LOG(MSG) << "MiniGraph Allocated: " << ToReadableSize(MiniGraphPool::TOTAL_ALLOCATED);

        uint64_t *VID_TO_DEG = new uint64_t [graph->get_vnum()];
        uint64_t *VID_TO_OFFSET = graph->m_offset;
        uint64_t *VID_TO_TRIANGLES = graph->m_triangles;
        for (IdType i = 0; i < graph->get_vnum(); i++) {
            VID_TO_DEG[i] = graph->Degree(i);
        }
        ctx.profiler->save_aggr_csv(prof_aggr_path, VID_TO_DEG, VID_TO_OFFSET, VID_TO_TRIANGLES);
        ctx.profiler->save_loop_csv(prof_loop_path);
        delete[] VID_TO_DEG;
    }
    log.save(PROJECT_LOG_DIR);
}
