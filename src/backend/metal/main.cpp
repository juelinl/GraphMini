#include "backend.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace minigraph::metal;
static size_t argument(const char *text, size_t maximum) {
    const std::string value(text);
    if (value.empty() || !std::all_of(value.begin(),value.end(),[](char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("Expected a nonnegative integer argument");
    const auto n = std::stoull(value);
    if (n > maximum) throw std::invalid_argument("Numeric argument exceeds supported range");
    return size_t(n);
}
int main(int argc, char **argv) {
    try {
        const auto start = std::chrono::steady_clock::now();
        Backend backend;
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            Adjacency graph(8);
            for (uint32_t i = 0; i < 8; ++i)
                for (uint32_t j = 0; j < 8; ++j) if (i != j) graph[i].push_back(j);
            for (auto [k,expected] : std::vector<std::pair<unsigned,uint64_t>>{{3,56},{6,28},{7,8},{8,1}}) {
                Options options; options.clique_size = k;
                if (backend.count_cliques(graph,options).matches != expected) throw std::runtime_error("Metal smoke count mismatch");
            }
            std::cout << "Metal smoke passed on " << backend.device_name() << '\n';
            return 0;
        }
        if (argc < 3 || argc > 6) throw std::invalid_argument("usage: graphmini_metal_cli graph.txt clique_size [repeats=3] [batch_MiB=64] [max_tasks=16384]");
        std::ifstream file(argv[1]);
        size_t n,m;
        if (!(file >> n >> m) || n > UINT32_MAX) throw std::invalid_argument("Invalid graph header");
        Adjacency graph(n);
        for (size_t i = 0; i < m; ++i) {
            uint32_t u,v;
            if (!(file >> u >> v) || u >= n || v >= n || u == v) throw std::invalid_argument("Invalid edge");
            graph[u].push_back(v); graph[v].push_back(u);
        }
        for (auto &row : graph) {
            std::sort(row.begin(),row.end()); row.erase(std::unique(row.begin(),row.end()),row.end());
        }
        Options options; options.clique_size = unsigned(argument(argv[2],8));
        unsigned repeats = argc > 3 ? unsigned(argument(argv[3],100)) : 3;
        if (!repeats || repeats > 100) throw std::invalid_argument("repeats must be 1..100");
        if (argc > 4) options.batch_bytes = argument(argv[4],std::numeric_limits<size_t>::max()/(1024*1024))*1024*1024;
        if (argc > 5) options.max_tasks = argument(argv[5],UINT32_MAX);
        uint64_t expected = 0;
        for (unsigned trial = 0; trial <= repeats; ++trial) {
            const Result r = backend.count_cliques(graph,options);
            if (trial && r.matches != expected) throw std::runtime_error("Unstable Metal count");
            expected = r.matches;
            std::cout << "{\"k\":" << options.clique_size << ",\"trial\":" << trial
                      << ",\"matches\":" << r.matches << ",\"total_ms\":" << r.total_seconds*1000
                      << ",\"validation_ms\":" << r.validation_seconds*1000
                      << ",\"construction_ms\":" << r.construction_seconds*1000
                      << ",\"dispatch_wait_ms\":" << r.dispatch_wait_seconds*1000
                      << ",\"gpu_ms\":" << r.gpu_seconds*1000
                      << ",\"regions\":" << r.regions << ",\"tasks\":" << r.tasks
                      << ",\"batches\":" << r.batches << ",\"peak_buffer_bytes\":" << r.peak_buffer_bytes << "}\n" << std::flush;
        }
        std::cerr << "Device: " << backend.device_name() << "; process elapsed seconds: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
