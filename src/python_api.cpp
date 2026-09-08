#include "codegen.h"
#include "compilation_profile.h"
#include "backend/backend.h"
#include "common.h"
#include "configure.h"
#include "graph_builder.h"
#include "plan_module.h"
#include "timer.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <oneapi/tbb/global_control.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace py = pybind11;

namespace minigraph {
namespace {

int default_thread_count() {
    const unsigned int detected = std::thread::hardware_concurrency();
    return static_cast<int>(std::max(1u, detected));
}

std::string normalize_token(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(value.begin(), value.end(), '-', '_');
    return value;
}

AdjMatType parse_adjmat_type(const std::string &value) {
    const std::string lowered = normalize_token(value);
    if (lowered == "vertex" || lowered == "vertex_induced") {
        return AdjMatType::VertexInduced;
    }
    if (lowered == "edge" || lowered == "edge_induced") {
        return AdjMatType::EdgeInduced;
    }
    if (lowered == "edge_iep" || lowered == "edge_induced_iep") {
        return AdjMatType::EdgeInducedIEP;
    }
    throw std::invalid_argument("Invalid query_type: " + value);
}

PruningType parse_pruning_type(const std::string &value) {
    const std::string lowered = normalize_token(value);
    if (lowered == "none") {
        return PruningType::None;
    }
    if (lowered == "eager") {
        return PruningType::Eager;
    }
    if (lowered == "costmodel" || lowered == "cost_model") {
        return PruningType::CostModel;
    }
    throw std::invalid_argument("Invalid pruning_type: " + value + ". Python API supports: none, eager, costmodel");
}

ParallelType parse_parallel_type(const std::string &value) {
    const std::string lowered = normalize_token(value);
    if (lowered == "openmp") {
        return ParallelType::OpenMP;
    }
    if (lowered == "tbbtop" || lowered == "tbb_top") {
        return ParallelType::TbbTop;
    }
    if (lowered == "nested") {
        return ParallelType::Nested;
    }
    if (lowered == "nestedrt" || lowered == "nested_rt") {
        return ParallelType::NestedRt;
    }
    throw std::invalid_argument("Invalid parallel_type: " + value);
}

SchedulerType parse_scheduler_type(const std::string &value) {
    const std::string lowered = normalize_token(value);
    if (lowered == "graphpi") {
        return SchedulerType::GraphPi;
    }
    if (lowered == "graphmini") {
        return SchedulerType::GraphMini;
    }
    if (lowered == "graphzero") {
        return SchedulerType::GraphZero;
    }
    if (lowered == "outgoing") {
        return SchedulerType::Outgoing;
    }
    if (lowered == "bitmap_balanced") {
        return SchedulerType::BitmapBalanced;
    }
    throw std::invalid_argument("Invalid scheduler: " + value);
}

template <typename T>
std::vector<T> numpy_vector(py::array_t<T, py::array::c_style | py::array::forcecast> array,
                            const char *name) {
    if (array.ndim() != 1) {
        throw std::invalid_argument(std::string(name) + " must be a 1-D array");
    }
    const auto *begin = array.data();
    return std::vector<T>(begin, begin + array.shape(0));
}

template <typename T>
std::vector<T> optional_numpy_vector(const py::object &obj, const char *name) {
    if (obj.is_none()) {
        return {};
    }
    return numpy_vector<T>(py::cast<py::array_t<T, py::array::c_style | py::array::forcecast>>(obj), name);
}

std::string hash_code_string(const std::string &code) {
    const size_t hash_value = std::hash<std::string>{}(code);
    std::ostringstream out;
    out << std::hex << hash_value;
    return out.str();
}

std::filesystem::path plan_cache_dir() {
    std::filesystem::path cache_dir(PROJECT_BINARY_DIR);
    cache_dir /= GRAPHMINI_EXPERIMENTAL_BACKEND_MODULE ? "python_plan_cache_backend_module"
                 : GRAPHMINI_EXPERIMENTAL_TBB_MODULE ? "python_plan_cache_tbb_module"
                 : GRAPHMINI_EXPERIMENTAL_HEADER_UNITS ? "python_plan_cache_header_units"
                                                       : "python_plan_cache";
    cache_dir /= "tbb-" GRAPHMINI_TBB_VERSION;
    cache_dir /= GRAPHMINI_PLAN_BUILD_ID;
    std::filesystem::create_directories(cache_dir);
    return cache_dir;
}

std::filesystem::path cached_module_copy_path(const std::string &cache_key) {
    std::filesystem::path path = plan_cache_dir();
    path /= cache_key + generated_plan_module_path().extension().string();
    return path;
}

struct RunResult {
    long long number_of_matches{0};
    double execution_time_seconds{0.0};
    double throughput{0.0};
    int num_threads{1};
    uint64_t vertex_allocated{0};
    uint64_t minigraph_allocated{0};
    double thread_min_time_seconds{0.0};
    double thread_mean_time_seconds{0.0};
    double thread_max_time_seconds{0.0};
    double thread_time_std_seconds{0.0};
};

std::string format_run_result(const RunResult &result) {
    std::ostringstream out;
    out << "RunResult(number_of_matches=" << result.number_of_matches
        << ", execution_time_seconds=" << result.execution_time_seconds
        << ", throughput=" << result.throughput
        << ", num_threads=" << result.num_threads
        << ", vertex_allocated=" << result.vertex_allocated
        << ", minigraph_allocated=" << result.minigraph_allocated
        << ", thread_min_time_seconds=" << result.thread_min_time_seconds
        << ", thread_mean_time_seconds=" << result.thread_mean_time_seconds
        << ", thread_max_time_seconds=" << result.thread_max_time_seconds
        << ", thread_time_std_seconds=" << result.thread_time_std_seconds
        << ")";
    return out.str();
}

class PyGraph {
public:
    explicit PyGraph(std::shared_ptr<Graph> graph) : graph_(std::move(graph)) {}

    static PyGraph from_preprocessed(const std::string &graph_dir, bool reorder_by_degree) {
        return PyGraph(std::shared_ptr<Graph>(load_graph_from_preprocessed(graph_dir, reorder_by_degree).release()));
    }

    static PyGraph from_csr(py::array_t<uint64_t, py::array::c_style | py::array::forcecast> indptr,
                            py::array_t<IdType, py::array::c_style | py::array::forcecast> indices,
                            py::object offsets,
                            py::object triangles,
                            bool reorder_by_degree) {
        GraphCSRData data;
        data.indptr = numpy_vector<uint64_t>(indptr, "indptr");
        data.indices = numpy_vector<IdType>(indices, "indices");
        data.offsets = optional_numpy_vector<uint64_t>(offsets, "offsets");
        data.triangles = optional_numpy_vector<uint64_t>(triangles, "triangles");
        return PyGraph(std::shared_ptr<Graph>(build_graph_from_csr(std::move(data), reorder_by_degree).release()));
    }

    const Graph &graph() const { return *graph_; }
    const std::shared_ptr<Graph> &ptr() const { return graph_; }

private:
    std::shared_ptr<Graph> graph_;
};

class CompiledPlan {
public:
    CompiledPlan(std::shared_ptr<Graph> graph,
                 std::string query_adjmat,
                 std::string query_type,
                 std::string pruning_type,
                 std::string parallel_type,
                 std::string scheduler,
                 bool bitmap = false,
                 bool bitmap_diagnostics = false)
            : graph_(std::move(graph)),
              query_adjmat_(std::move(query_adjmat)),
              query_type_(std::move(query_type)),
              pruning_type_(std::move(pruning_type)),
              parallel_type_(std::move(parallel_type)),
              scheduler_(std::move(scheduler)), bitmap_(bitmap), bitmap_diagnostics_(bitmap_diagnostics) {
        compile();
    }

    uint64_t pattern_size() const { return module_.pattern_size(); }
    const std::filesystem::path &module_path() const { return module_copy_path_; }
    const std::string &generated_code() const { return generated_code_; }
    const CompilationProfile &compilation_profile() const { return compilation_profile_; }

    RunResult run(const std::shared_ptr<Graph> &graph, int num_threads) const {
        if (num_threads <= 0) {
            num_threads = default_thread_count();
        }

        internal::VertexSetPool::TOTAL_ALLOCATED = 0;
        MiniGraphPool::TOTAL_ALLOCATED = 0;

        Timer timer;
        tbb::global_control tbb_thread_limit(tbb::global_control::max_allowed_parallelism, num_threads);
        const int context_capacity = std::max(num_threads, 256);
        Context ctx(context_capacity);
        // Capacity accommodates TBB worker IDs; it is not the OpenMP team size.
        ctx.num_threads = num_threads;
        module_.run(graph.get(), ctx);

        RunResult out;
        out.number_of_matches = ctx.get_result();
        out.execution_time_seconds = timer.Passed();
        out.throughput = out.execution_time_seconds > 0.0
                         ? static_cast<double>(out.number_of_matches) / out.execution_time_seconds
                         : 0.0;
        out.num_threads = num_threads;
        out.vertex_allocated = internal::VertexSetPool::TOTAL_ALLOCATED;
        out.minigraph_allocated = MiniGraphPool::TOTAL_ALLOCATED;

        std::vector<double> active_times;
        active_times.reserve(ctx.per_thread_time.size());
        for (size_t i = 0; i < ctx.per_thread_time.size(); ++i) {
            const double value = ctx.per_thread_time[i] + ctx.tick_time(i);
            if (value > 0.0) {
                active_times.push_back(value);
            }
        }
        if (active_times.empty()) {
            out.thread_min_time_seconds = 0.0;
            out.thread_mean_time_seconds = 0.0;
            out.thread_max_time_seconds = 0.0;
            out.thread_time_std_seconds = 0.0;
        } else {
            out.thread_min_time_seconds = *std::min_element(active_times.begin(), active_times.end());
            out.thread_max_time_seconds = *std::max_element(active_times.begin(), active_times.end());
            const double sum = std::accumulate(active_times.begin(), active_times.end(), 0.0);
            out.thread_mean_time_seconds = sum / static_cast<double>(active_times.size());
            double variance = 0.0;
            for (double value: active_times) {
                const double delta = value - out.thread_mean_time_seconds;
                variance += delta * delta;
            }
            out.thread_time_std_seconds = std::sqrt(variance / static_cast<double>(active_times.size()));
        }
        return out;
    }

private:
    void compile() {
        CompilationCapture capture(compilation_profile_);
        CompilationStage total("compile_total");
        CodeGenConfig config;
        MetaData meta;
        {
        CompilationStage stage("metadata_and_config");
        config.adjMatType = parse_adjmat_type(query_type_);
        config.pruningType = parse_pruning_type(pruning_type_);
        config.parType = parse_parallel_type(parallel_type_);
        config.schedulerType = parse_scheduler_type(scheduler_);
        config.runnerType = RunnerType::Benchmark;
        config.bitmap = bitmap_;
        config.bitmapDiagnostics = bitmap_diagnostics_;

        meta = metadata_from_graph(*graph_);
        }
        {
        CompilationStage stage("codegen_total");
        generated_code_ = gen_code(query_adjmat_, config, meta);
        }
        {
        CompilationStage stage("cache_lookup");
        const std::string cache_key = hash_code_string(generated_code_);
        module_copy_path_ = cached_module_copy_path(cache_key);
        compilation_profile_.cache_hit = std::filesystem::exists(module_copy_path_);
        }
        if (!compilation_profile_.cache_hit) {
            {
            CompilationStage stage("source_write");
            std::ofstream out(generated_plan_source_path());
            out << generated_code_;
            out.close();
            }
            {
            CompilationStage stage("build");
            build_generated_plan_module(PROJECT_BINARY_DIR);
            }
            {
            CompilationStage stage("library_copy");
            std::filesystem::copy_file(generated_plan_module_path(),
                                       module_copy_path_,
                                       std::filesystem::copy_options::overwrite_existing);
            }
        }
        CompilationStage load("library_load");
        module_ = LoadedPlanModule(module_copy_path_);
    }

    std::shared_ptr<Graph> graph_;
    std::string query_adjmat_;
    std::string query_type_;
    std::string pruning_type_;
    std::string parallel_type_;
    std::string scheduler_;
    bool bitmap_;
    bool bitmap_diagnostics_;
    std::string generated_code_;
    std::filesystem::path module_copy_path_;
    LoadedPlanModule module_;
    CompilationProfile compilation_profile_;
};

} // namespace
} // namespace minigraph

PYBIND11_MODULE(graphmini, m) {
    using namespace minigraph;

    py::class_<RunResult>(m, "RunResult")
            .def_readonly("number_of_matches", &RunResult::number_of_matches)
            .def_property_readonly("result", [](const RunResult &self) {
                return self.number_of_matches;
            })
            .def_readonly("execution_time_seconds", &RunResult::execution_time_seconds)
            .def_readonly("throughput", &RunResult::throughput)
            .def_readonly("num_threads", &RunResult::num_threads)
            .def_readonly("vertex_allocated", &RunResult::vertex_allocated)
            .def_readonly("minigraph_allocated", &RunResult::minigraph_allocated)
            .def_readonly("thread_min_time_seconds", &RunResult::thread_min_time_seconds)
            .def_readonly("thread_mean_time_seconds", &RunResult::thread_mean_time_seconds)
            .def_readonly("thread_max_time_seconds", &RunResult::thread_max_time_seconds)
            .def_readonly("thread_time_std_seconds", &RunResult::thread_time_std_seconds)
            .def("__repr__", [](const RunResult &self) {
                return format_run_result(self);
            });

    py::class_<PyGraph>(m, "Graph")
            .def_static("from_preprocessed", &PyGraph::from_preprocessed,
                        py::arg("graph_dir"),
                        py::arg("reorder_by_degree") = false)
            .def_static("from_csr", &PyGraph::from_csr,
                        py::arg("indptr"),
                        py::arg("indices"),
                        py::arg("offsets") = py::none(),
                        py::arg("triangles") = py::none(),
                        py::arg("reorder_by_degree") = false)
            .def_property_readonly("num_vertices", [](const PyGraph &self) {
                return self.graph().num_vertex;
            })
            .def_property_readonly("num_edges", [](const PyGraph &self) {
                return self.graph().num_edge;
            })
            .def_property_readonly("num_triangles", [](const PyGraph &self) {
                return self.graph().num_triangle;
            });

    py::class_<CompiledPlan>(m, "CompiledPlan")
            .def(py::init([](const PyGraph &graph,
                             const std::string &query_adjmat,
                             const std::string &query_type,
                             const std::string &pruning_type,
                             const std::string &parallel_type,
                             const std::string &scheduler,
                             bool bitmap, bool bitmap_diagnostics) {
                     return CompiledPlan(graph.ptr(),
                                         query_adjmat,
                                         query_type,
                                         pruning_type,
                                         parallel_type,
                                         scheduler, bitmap, bitmap_diagnostics);
                 }),
                 py::arg("graph"),
                 py::arg("query_adjmat"),
                 py::arg("query_type"),
                 py::arg("pruning_type") = "eager",
                 py::arg("parallel_type") = "nested_rt",
                 py::arg("scheduler") = "graphpi",
                 py::arg("bitmap") = false,
                 py::arg("bitmap_diagnostics") = false)
            .def("run", [](const CompiledPlan &self, const PyGraph &graph, int num_threads) {
                py::gil_scoped_release release;
                return self.run(graph.ptr(), num_threads);
            }, py::arg("graph"), py::arg("num_threads") = 0)
            .def_property_readonly("pattern_size", &CompiledPlan::pattern_size)
            .def_property_readonly("module_path", [](const CompiledPlan &self) {
                return self.module_path().string();
            })
            .def_property_readonly("generated_code", &CompiledPlan::generated_code)
            .def_property_readonly("compilation_profile", [](const CompiledPlan &self) {
                py::dict out;
                out["seconds"] = self.compilation_profile().seconds;
                out["cache_hit"] = self.compilation_profile().cache_hit;
                return out;
            });

    m.def("compile_plan",
          [](const PyGraph &graph,
             const std::string &query_adjmat,
             const std::string &query_type,
             const std::string &pruning_type,
             const std::string &parallel_type,
             const std::string &scheduler,
             bool bitmap, bool bitmap_diagnostics) {
              py::gil_scoped_release release;
              return CompiledPlan(graph.ptr(),
                                  query_adjmat,
                                  query_type,
                                  pruning_type,
                                  parallel_type,
                                  scheduler, bitmap, bitmap_diagnostics);
          },
          py::arg("graph"),
          py::arg("query_adjmat"),
          py::arg("query_type"),
          py::arg("pruning_type") = "eager",
          py::arg("parallel_type") = "nested_rt",
          py::arg("scheduler") = "graphpi",
          py::arg("bitmap") = false,
          py::arg("bitmap_diagnostics") = false);
}
