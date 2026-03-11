//
// Created by ubuntu on 1/1/23.
//
#include "codegen.h"
#include "logging.h"
#include "configure.h"
#include "common.h"
#include <cxxopts.hpp>
#include <vector>
#include <iostream>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <chrono>
#include <thread>
using namespace minigraph;

namespace {
int default_thread_count() {
    const unsigned int detected = std::thread::hardware_concurrency();
    return std::max(1u, detected);
}

struct QueryInspection {
    int pattern_size{0};
    std::vector<int> self_loop_vertices;
    std::vector<std::vector<int>> components;
};

std::string format_node_id(int node) {
    return fmt::format("n{}", node);
}

std::string format_node_list(const std::vector<int> &nodes) {
    std::ostringstream out;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << format_node_id(nodes[i]);
    }
    return out.str();
}

bool inspect_query_connectivity(const std::string &adj_mat, QueryInspection &inspection, std::string &error) {
    const int pattern_size = static_cast<int>(std::sqrt(static_cast<double>(adj_mat.size())));
    if (pattern_size < 1 || pattern_size * pattern_size != static_cast<int>(adj_mat.size())) {
        error = "Invalid query adjacency matrix: length must be a non-zero square number.";
        return false;
    }

    inspection = QueryInspection{};
    inspection.pattern_size = pattern_size;
    auto at = [&](int row, int col) {
        return adj_mat[static_cast<size_t>(row * pattern_size + col)];
    };

    for (int vertex = 0; vertex < pattern_size; ++vertex) {
        if (at(vertex, vertex) == '1') {
            inspection.self_loop_vertices.push_back(vertex);
        }
    }

    std::vector<char> visited(static_cast<size_t>(pattern_size), 0);
    for (int start = 0; start < pattern_size; ++start) {
        if (visited[static_cast<size_t>(start)] != 0) {
            continue;
        }

        std::vector<int> component;
        std::vector<int> queue = {start};
        visited[static_cast<size_t>(start)] = 1;
        for (size_t head = 0; head < queue.size(); ++head) {
            const int node = queue[head];
            component.push_back(node);
            for (int other = 0; other < pattern_size; ++other) {
                if (node == other || visited[static_cast<size_t>(other)] != 0) {
                    continue;
                }
                if (at(node, other) == '1' || at(other, node) == '1') {
                    visited[static_cast<size_t>(other)] = 1;
                    queue.push_back(other);
                }
            }
        }
        std::sort(component.begin(), component.end());
        inspection.components.push_back(std::move(component));
    }

    std::sort(inspection.components.begin(), inspection.components.end(),
              [](const std::vector<int> &lhs, const std::vector<int> &rhs) {
                  return lhs.front() < rhs.front();
              });

    if (inspection.components.size() <= 1) {
        return true;
    }

    std::ostringstream out;
    out << "Invalid query pattern: the query graph is disconnected after ignoring diagonal self-loops.\n";
    out << "Pattern Size: " << inspection.pattern_size << '\n';
    if (!inspection.self_loop_vertices.empty()) {
        out << "Diagonal Self-Loops Ignored On: " << format_node_list(inspection.self_loop_vertices) << '\n';
    }
    out << "Connected Components:\n";
    for (size_t i = 0; i < inspection.components.size(); ++i) {
        out << "  " << (i + 1) << ". " << format_node_list(inspection.components[i]) << '\n';
    }
    error = out.str();
    return false;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalize_token(std::string value) {
    value = to_lower(std::move(value));
    std::replace(value.begin(), value.end(), '-', '_');
    return value;
}

const char *scheduler_type_name(SchedulerType scheduler_type) {
    switch (scheduler_type) {
        case SchedulerType::GraphPi:
            return "graphpi";
        case SchedulerType::GraphMini:
            return "graphmini";
        case SchedulerType::GraphZero:
            return "graphzero";
    }
    return "graphmini";
}

bool try_parse_scheduler_type(const std::string &value, SchedulerType &scheduler_type) {
    const std::string lowered = normalize_token(value);
    if (lowered == "graphpi") {
        scheduler_type = SchedulerType::GraphPi;
        return true;
    }
    if (lowered == "graphmini") {
        scheduler_type = SchedulerType::GraphMini;
        return true;
    }
    if (lowered == "graphzero") {
        scheduler_type = SchedulerType::GraphZero;
        return true;
    }
    return false;
}

bool try_parse_adjmat_type(const std::string &value, AdjMatType &adjmat_type) {
    const std::string lowered = normalize_token(value);
    if (lowered == "vertex" || lowered == "vertex_induced") {
        adjmat_type = AdjMatType::VertexInduced;
        return true;
    }
    if (lowered == "edge" || lowered == "edge_induced") {
        adjmat_type = AdjMatType::EdgeInduced;
        return true;
    }
    if (lowered == "edge_iep" || lowered == "edge_induced_iep") {
        adjmat_type = AdjMatType::EdgeInducedIEP;
        return true;
    }
    return false;
}

bool try_parse_pruning_type(const std::string &value, PruningType &pruning_type) {
    const std::string lowered = normalize_token(value);
    if (lowered == "none") {
        pruning_type = PruningType::None;
        return true;
    }
    if (lowered == "static") {
        pruning_type = PruningType::Static;
        return true;
    }
    if (lowered == "eager") {
        pruning_type = PruningType::Eager;
        return true;
    }
    if (lowered == "online") {
        pruning_type = PruningType::Online;
        return true;
    }
    if (lowered == "costmodel" || lowered == "cost_model") {
        pruning_type = PruningType::CostModel;
        return true;
    }
    return false;
}

bool try_parse_parallel_type(const std::string &value, ParallelType &parallel_type) {
    const std::string lowered = normalize_token(value);
    if (lowered == "openmp") {
        parallel_type = ParallelType::OpenMP;
        return true;
    }
    if (lowered == "tbbtop" || lowered == "tbb_top") {
        parallel_type = ParallelType::TbbTop;
        return true;
    }
    if (lowered == "nested") {
        parallel_type = ParallelType::Nested;
        return true;
    }
    if (lowered == "nestedrt" || lowered == "nested_rt") {
        parallel_type = ParallelType::NestedRt;
        return true;
    }
    return false;
}

std::string build_run_help(const cxxopts::Options &) {
    std::ostringstream out;
    out << "Generate, compile, and run a plan for one query.\n\n";
    out << "Usage:\n";
    out << "  ./build/bin/run --graph_name=<name> --path_to_graph=<dir> --query_name=<name> "
           "--query_adjmat=<adjmat> --query_type=<vertex|edge|edge_iep> "
           "--pruning_type=<none|static|eager|online|costmodel> "
           "--parallel_type=<openmp|tbb_top|nested|nested_rt> "
           "[--scheduler=<graphpi|graphmini|graphzero>] [--num_threads=<count>] [--exp_id=<id>]\n\n";
    out << "Required Options:\n";
    out << "  --graph_name      Graph nickname.\n";
    out << "  --path_to_graph   Path to the preprocessed graph directory.\n";
    out << "  --query_name      Query nickname.\n";
    out << "  --query_adjmat    Query adjacency matrix string.\n";
    out << "  --query_type      One of: vertex, edge, edge_iep.\n";
    out << "  --pruning_type    One of: none, static, eager, online, costmodel.\n";
    out << "  --parallel_type   One of: openmp, tbb_top, nested, nested_rt.\n\n";
    out << "Optional Options:\n";
    out << "  --scheduler       One of: graphpi, graphmini, graphzero. Default: graphmini.\n";
    out << "  --num_threads     Positive thread count. Default: all available threads.\n";
    out << "  --exp_id          Experiment id for logging. Default: -1.\n";
    out << "  --help            Show this help text.\n\n";
    out << "Positional Compatibility:\n";
    out << "  You may also pass the seven required options positionally in this order:\n";
    out << "  <graph_name> <path_to_graph> <query_name> <query_adjmat> <query_type> <pruning_type> <parallel_type>\n\n";
    out << "Examples:\n";
    out << "  ./build/bin/run --graph_name=wiki --path_to_graph=./dataset/GraphMini/wiki "
           "--query_name=P1 --query_adjmat=0111101111011110 --query_type=vertex "
           "--pruning_type=costmodel --parallel_type=nested_rt --scheduler=graphmini --num_threads=32\n";
    out << "  ./build/bin/run wiki ./dataset/GraphMini/wiki P1 0111101111011110 vertex costmodel nested_rt "
           "--scheduler=graphpi --exp_id=42\n";
    return out.str();
}
} // namespace

std::string exec(const char *cmd) {
    struct PipeCloser {
        void operator()(FILE *pipe) const {
            if (pipe != nullptr) {
                pclose(pipe);
            }
        }
    };

    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd, "r"));
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}


struct AppConfig {
    int exp_id;
    int num_threads;
    CodeGenConfig codegen;
    std::string data_name;
    std::string pattern_name;
    std::string pat;
    std::string graph_dir;
};

std::filesystem::path output_dir(AppConfig config) {
    std::filesystem::path out = PROJECT_LOG_DIR;
    out /= config.data_name;
    out /= config.pattern_name;
    if (config.codegen.adjMatType == AdjMatType::EdgeInduced) {
        out /= "edge_induced";
    } else if (config.codegen.adjMatType == AdjMatType::VertexInduced) {
        out /= "vertex_induced";
    } else {
        out /= "edge_induced_iep";
    };

    return out;
}

std::filesystem::path output_log(AppConfig config) {
    auto out = output_dir(config);
    std::string prefix;
    if (config.codegen.parType == ParallelType::OpenMP) prefix = "omp_";
    if (config.codegen.parType == ParallelType::TbbTop) prefix = "tbb_top_";
    if (config.codegen.parType == ParallelType::Nested) prefix = "tbb_nested_";
    if (config.codegen.parType == ParallelType::NestedRt) prefix = "tbb_nested_rt_";
    if (config.codegen.pruningType == PruningType::None) {
        out /= prefix + "baseline.txt";
    } else if (config.codegen.pruningType == PruningType::Eager) {
        out /= prefix + "eager.txt";
    } else if (config.codegen.pruningType == PruningType::Static) {
        out /= prefix + "lazy.txt";
    } else if (config.codegen.pruningType == PruningType::Online) {
        out /= prefix + "online.txt";
    } else if (config.codegen.pruningType == PruningType::CostModel) {
        out /= prefix + "costmodel.txt";
    }
    return out;
}

std::filesystem::path code_path() {
    std::filesystem::path code_file(PROJECT_SOURCE_DIR);
    code_file /= "src";
    code_file /= "codegen_output";
    code_file /= "plan.cpp";
    return code_file;
}

void compile(AppConfig config) {
    CompilerLog log;
    MetaData meta;
    meta.read(config.graph_dir);
    Timer t;
    LOG(MSG) << "Scheduler: " << scheduler_type_name(config.codegen.schedulerType);
    std::string code = gen_code(config.pat, config.codegen, meta);
    auto codegen_t = t.Passed();
    t.Reset();
    std::ofstream out_file(code_path());

    LOG(MSG) << "Generated Code Path: " << code_path();
    out_file << code;
    out_file.flush();
    out_file.close();
    auto codewrite_t = t.Passed();

    // format code
    auto reformat_cmd = fmt::format("clang-format -i {}", code_path().string());
    auto reformat_flag = system(reformat_cmd.c_str());
    if (reformat_flag != 0) {
        LOG(MSG) << "Install clang-format to format: " << code_path();
    }

    LOG(MSG) << "Code Generation Time: " << ToReadableDuration(codewrite_t + codegen_t);

    // compile and run
    auto compile_cmd = fmt::format("cmake --build {compile_path} --target runner 1>>/dev/null 2>>/dev/null",
                                   fmt::arg("compile_path", PROJECT_BINARY_DIR));
    // LOG(MSG) << "CMD: " << compile_cmd;
    t.Reset();
    int flag = system(compile_cmd.c_str());
    int num_try = 3;
    while (num_try > 0 && flag != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        flag = system(compile_cmd.c_str());
        num_try--;
    }
    if (flag != 0) exit(-1 && "compilation error");
    auto compile_t = t.Passed();
    LOG(MSG) << "Compilation Time: " << ToReadableDuration(compile_t);
    auto mkdir_cmd = fmt::format("mkdir -p {bin_dir}",
                                 fmt::arg("bin_dir", PROJECT_PLAN_DIR));
    // LOG(MSG) << "CMD: " << mkdir_cmd;
    flag = system(mkdir_cmd.c_str());
    if (flag != 0) exit(-1 && "mkdir error");

    std::filesystem::path bin_path = std::filesystem::path(CMAKE_RUNTIME_OUTPUT_DIRECTORY) / "runner";
    std::filesystem::path dst_path = std::filesystem::path(PROJECT_PLAN_DIR) / std::to_string(config.exp_id);
    auto mv_cmd = fmt::format("mv {bin_path} {dst_path}",
                              fmt::arg("bin_path", bin_path.string()),
                              fmt::arg("dst_path", dst_path.string()));

    flag = system(mv_cmd.c_str());
    if (flag != 0) exit(-1 && "mv error");

    log.expId = config.exp_id;
    log.patternSize = sqrt(config.pat.size());
    log.compileTime = compile_t;
    log.codegenTime = codegen_t;
    log.schedulerType = config.codegen.schedulerType;
    log.pruningType = config.codegen.pruningType;
    log.parallelType = config.codegen.parType;
    log.adjMatType = config.codegen.adjMatType;
    log.patternAdj = config.pat;
    log.patternName = config.pattern_name;
    log.dataName = config.data_name;
    log.save(PROJECT_LOG_DIR);
};

void run(AppConfig config) {
    std::filesystem::path bin_path = std::filesystem::path(PROJECT_PLAN_DIR) / std::to_string(config.exp_id);
    auto run_cmd = fmt::format("{bin_path} {exp_id} {data_dir} {num_threads}",
                               fmt::arg("bin_path", bin_path.string()),
                               fmt::arg("data_dir", config.graph_dir),
                               fmt::arg("exp_id", config.exp_id),
                               fmt::arg("num_threads", config.num_threads));
    std::string run_results = exec(run_cmd.c_str());
    LOG(MSG) << run_results;
}

void compile_and_run(AppConfig config) {
    compile(config);
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(500ms);
    run(config);
}

int main(int argc, char *argv[]) {
    using namespace minigraph;
    cxxopts::Options options("run", "Generate, compile, and run a GraphMini plan.");
    options.positional_help("<graph_name> <path_to_graph> <query_name> <query_adjmat> <query_type> <pruning_type> <parallel_type>");
    options.add_options()
            ("graph_name", "Graph nickname", cxxopts::value<std::string>())
            ("path_to_graph", "Path to the preprocessed graph directory", cxxopts::value<std::string>())
            ("query_name", "Query nickname", cxxopts::value<std::string>())
            ("query_adjmat", "Query adjacency matrix string", cxxopts::value<std::string>())
            ("query_type", "Query type: vertex|edge|edge_iep", cxxopts::value<std::string>())
            ("pruning_type", "Pruning type: none|static|eager|online|costmodel", cxxopts::value<std::string>())
            ("parallel_type", "Parallel type: openmp|tbb_top|nested|nested_rt", cxxopts::value<std::string>())
            ("scheduler", "Scheduler: graphpi|graphmini|graphzero",
             cxxopts::value<std::string>()->default_value("graphmini"))
            ("num_threads", "Positive thread count. Default: all available threads",
             cxxopts::value<int>())
            ("exp_id", "Experiment id", cxxopts::value<int>()->default_value("-1"))
            ("help", "Show help");
    options.parse_positional({"graph_name", "path_to_graph", "query_name", "query_adjmat",
                              "query_type", "pruning_type", "parallel_type"});

    cxxopts::ParseResult parsed;
    try {
        parsed = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << "Error: " << e.what() << "\n\n" << build_run_help(options) << std::endl;
        return 1;
    }

    if (parsed.count("help")) {
        std::cout << build_run_help(options) << std::endl;
        return 0;
    }

    const char *required_options[] = {
            "graph_name",
            "path_to_graph",
            "query_name",
            "query_adjmat",
            "query_type",
            "pruning_type",
            "parallel_type",
    };
    for (const char *option: required_options) {
        if (parsed.count(option) == 0) {
            std::cerr << "Missing required option: --" << option << "\n\n"
                      << build_run_help(options) << std::endl;
            return 1;
        }
    }

    std::string graph_name = parsed["graph_name"].as<std::string>();
    std::string graph_dir = parsed["path_to_graph"].as<std::string>();
    std::string query_name = parsed["query_name"].as<std::string>();
    std::string query_str = parsed["query_adjmat"].as<std::string>();
    AdjMatType adjmat_type = AdjMatType::VertexInduced;
    PruningType prun_type = PruningType::Eager;
    ParallelType par_type = ParallelType::NestedRt;
    if (!try_parse_adjmat_type(parsed["query_type"].as<std::string>(), adjmat_type) ||
        !try_parse_pruning_type(parsed["pruning_type"].as<std::string>(), prun_type) ||
        !try_parse_parallel_type(parsed["parallel_type"].as<std::string>(), par_type)) {
        std::cerr << "Invalid query_type, pruning_type, or parallel_type.\n\n"
                  << build_run_help(options) << std::endl;
        return 1;
    }
    SchedulerType scheduler_type = SchedulerType::GraphMini;
    if (!try_parse_scheduler_type(parsed["scheduler"].as<std::string>(), scheduler_type)) {
        std::cerr << "Invalid scheduler.\n\n" << build_run_help(options) << std::endl;
        return 1;
    }
    int exp_id = parsed["exp_id"].as<int>();
    int num_threads = default_thread_count();
    if (parsed.count("num_threads") != 0) {
        num_threads = parsed["num_threads"].as<int>();
        if (num_threads <= 0) {
            std::cerr << "Invalid num_threads. It must be a positive integer.\n\n"
                      << build_run_help(options) << std::endl;
            return 1;
        }
    }

    CodeGenConfig conf;
    conf.adjMatType  = adjmat_type;
    conf.schedulerType = scheduler_type;
    conf.pruningType = prun_type;
    conf.parType     = par_type;

    AppConfig config;
    config.exp_id = exp_id;
    config.num_threads = num_threads;
    config.pattern_name = query_name;
    config.pat = query_str;
    config.codegen = conf;
    config.data_name = graph_name;
    config.graph_dir = graph_dir;

    QueryInspection inspection;
    std::string inspection_error;
    if (!inspect_query_connectivity(query_str, inspection, inspection_error)) {
        std::cerr << inspection_error << std::endl;
        return 1;
    }

    compile_and_run(config);
}
