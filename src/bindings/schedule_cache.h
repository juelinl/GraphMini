#pragma once

#include "compiler/planning.h"
#include "compiler_cache_id.h"
#include "configure.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace minigraph::python_internal {
namespace py = pybind11;

// Callers hold the GIL when constructing/comparing these JSON identities.
inline std::string canonical_json(const py::object &value) {
    return py::module_::import("json").attr("dumps")(
        value, py::arg("sort_keys") = true, py::arg("separators") = py::make_tuple(",", ":")).cast<std::string>();
}
inline std::string sha256(const std::string &value) {
    return py::module_::import("hashlib").attr("sha256")(py::bytes(value)).attr("hexdigest")().cast<std::string>();
}
inline py::dict schedule_identity(const ScheduleResult &schedule, bool induced) {
    auto restrictions = schedule.restrict_pair;
    std::sort(restrictions.begin(), restrictions.end());
    py::dict identity;
    identity["schema"] = "graphmini-outgoing-bitmap-v1";
    identity["scheduler"] = "outgoing";
    identity["semantics"] = induced ? "vertex" : "edge";
    identity["adjacency"] = schedule.adj_mat;
    identity["restrictions"] = restrictions;
    return identity;
}
inline py::dict schedule_counting(const ScheduleResult &schedule) {
    py::dict result;
    result["iep_num"] = schedule.iep_num;
    result["groups"] = schedule.iep_groups;
    result["values"] = schedule.iep_vals;
    result["redundancy"] = schedule.iep_redundancy;
    return result;
}
inline bool formatting_enabled() {
    const char *value = std::getenv("GRAPHMINI_FORMAT_CODE");
    return !value || std::string(value) != "0";
}
inline std::optional<std::string> read_artifact(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return {};
    return contents;
}
struct ScheduledKernel {
    std::string source;
    std::filesystem::path module;
};

inline std::optional<ScheduledKernel> lookup_scheduled_kernel(
    const ScheduleResult &schedule, const CodeGenConfig &config,
    const std::filesystem::path &cache_dir, const std::string &library_extension) {
    // Only the graph-independent catalog contract is eligible. Other configs
    // keep the exact-source cache, including explicit experimental settings.
    if (config.schedulerType != SchedulerType::Outgoing || config.pruningType != PruningType::None ||
        config.parType != ParallelType::NestedRt || config.runnerType != RunnerType::Benchmark ||
        config.bitmapDiagnostics || config.bitmapDeferredCounts) return {};
    py::gil_scoped_acquire acquire;
    try {
        const auto identity = schedule_identity(schedule, config.adjMatType == VertexInduced);
        const auto id = sha256(canonical_json(identity));
        const auto directory = std::filesystem::path(PROJECT_SOURCE_DIR) / "plans" /
            std::to_string(schedule.matching_order.size()) / id;
        const auto text = read_artifact(directory / "plan.json");
        if (!text) return {};
        const auto metadata = py::module_::import("json").attr("loads")(*text).cast<py::dict>();
        if (metadata["codegen_build_id"].cast<std::string>() != GRAPHMINI_CODEGEN_BUILD_ID ||
            metadata["format_enabled"].cast<bool>() != formatting_enabled() ||
            metadata["schedule_id"].cast<std::string>() != id) return {};
        py::dict stored_identity;
        for (auto item : identity) stored_identity[item.first] = metadata[item.first];
        if (canonical_json(stored_identity) != canonical_json(identity) ||
            canonical_json(metadata["schedule_counting"]) != canonical_json(schedule_counting(schedule))) return {};
        const auto execution_type = config.adjMatType == VertexInduced ? "vertex" :
            config.adjMatType == EdgeInducedIEP ? "edge_iep" : "edge";
        if (metadata["execution_query_type"].cast<std::string>() != execution_type) return {};
        py::dict options;
        options["pruning"] = "none";
        options["parallel"] = "nested_rt";
        options["bitmap"] = config.bitmap;
        options["bitmap_direct"] = config.bitmapDirect;
        options["bitmap_deferred_counts"] = false;
        if (canonical_json(metadata["config"]) != canonical_json(options)) return {};
        const auto source_file = metadata["source_file"].cast<std::string>();
        if (source_file != "bitmap.cpp" && source_file != "iep.cpp") return {};
        const auto hash = metadata["source_sha256"].cast<std::string>();
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
        const auto module = cache_dir / (hash + library_extension);
        if (!std::filesystem::is_regular_file(module)) return {};
        auto source = read_artifact(directory / source_file);
        if (!source || sha256(*source) != hash) return {};
        // cache_dir contains the runtime/compiler/CPU build ID; the independent
        // codegen ID above prevents stale source from an older emitter being used.
        return ScheduledKernel{std::move(*source), module};
    } catch (const std::exception &) {
        // A missing, stale, or malformed catalog is a normal cache miss.
        return {};
    }
}
} // namespace minigraph::python_internal
