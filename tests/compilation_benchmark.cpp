#include "codegen.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace minigraph;

int main(int argc, char **argv) {
  if (argc < 2)
    throw std::invalid_argument(
        "Usage: compilation_benchmark OUTPUT_DIRECTORY [SIZE ...]");
  std::filesystem::create_directories(argv[1]);
  std::vector<int> sizes;
  for (int i = 2; i < argc; ++i) {
    const int size = std::stoi(argv[i]);
    if (size < 4 || size > 7)
      throw std::invalid_argument(
          "Benchmark sizes must be between four and seven");
    sizes.push_back(size);
  }
  if (sizes.empty())
    sizes.push_back(4);
  const MetaData meta(100, 1000, 600, 30, 20, 60);
  for (int size : sizes) {
    for (bool nested : {false, true}) {
      for (const std::string family : {"clique", "star", "cycle"}) {
        const bool star = family == "star";
        CodeGenConfig config;
        config.adjMatType = star ? EdgeInducedIEP : EdgeInduced;
        config.schedulerType = SchedulerType::GraphPi;
        config.pruningType =
            nested ? PruningType::CostModel : PruningType::None;
        config.parType = nested ? ParallelType::NestedRt : ParallelType::OpenMP;
        config.runnerType = RunnerType::Benchmark;
        std::string query(size * size, '0');
        for (int i = 0; i < size; ++i)
          for (int j = i + 1; j < size; ++j)
            if (family == "clique" || (star && i == 0) ||
                (family == "cycle" &&
                 (j == i + 1 || (i == 0 && j == size - 1))))
              query[i * size + j] = query[j * size + i] = '1';
        const std::string name =
            family + std::to_string(size) +
            (nested ? "_nested_costmodel" : "_openmp_none");
        std::string code;
        constexpr int repeats = 20;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < repeats; ++i)
          code = gen_code(query, config, meta);
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count() /
                               repeats;
        std::ofstream(std::filesystem::path(argv[1]) / (name + ".cpp")) << code;
        std::cout << "BENCHMARK " << name << ' ' << seconds << '\n';
      }
    }
  }
}
