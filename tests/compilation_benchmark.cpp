#include "codegen.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace minigraph;

int main(int argc, char **argv) {
    if (argc != 2)
        throw std::invalid_argument("Usage: compilation_benchmark OUTPUT_DIRECTORY");
    std::filesystem::create_directories(argv[1]);
    const MetaData meta(100, 1000, 600, 30, 20, 60);
    for (bool nested : {false, true}) {
        for (bool star : {false, true}) {
            CodeGenConfig config;
            config.adjMatType = star ? EdgeInducedIEP : EdgeInduced;
            config.schedulerType = SchedulerType::GraphPi;
            config.pruningType = nested ? PruningType::CostModel : PruningType::None;
            config.parType = nested ? ParallelType::NestedRt : ParallelType::OpenMP;
            config.runnerType = RunnerType::Benchmark;
            const std::string query = star ? "0111100010001000" : "0111101111011110";
            const std::string name = std::string(star ? "star" : "clique") +
                                     (nested ? "_nested_costmodel" : "_openmp_none");
            std::string code;
            constexpr int repeats = 20;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < repeats; ++i)
                code = gen_code(query, config, meta);
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count() / repeats;
            std::ofstream(std::filesystem::path(argv[1]) / (name + ".cpp")) << code;
            std::cout << "BENCHMARK " << name << ' ' << seconds << '\n';
        }
    }
}
