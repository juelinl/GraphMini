#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace minigraph {

struct Graph;
struct Context;

inline constexpr const char *kGeneratedPlanEntrypoint = "graphmini_plan";
inline constexpr const char *kGeneratedPlanPatternSizeEntrypoint = "graphmini_pattern_size";

class LoadedPlanModule {
public:
    LoadedPlanModule() = default;
    explicit LoadedPlanModule(const std::filesystem::path &module_path);
    ~LoadedPlanModule();

    LoadedPlanModule(const LoadedPlanModule &) = delete;
    LoadedPlanModule &operator=(const LoadedPlanModule &) = delete;

    LoadedPlanModule(LoadedPlanModule &&other) noexcept;
    LoadedPlanModule &operator=(LoadedPlanModule &&other) noexcept;

    bool loaded() const { return handle_ != nullptr; }
    uint64_t pattern_size() const;
    void run(const Graph *graph, Context &ctx) const;
    const std::filesystem::path &path() const { return module_path_; }

private:
    using PlanEntrypoint = void (*)(const Graph *, Context *);
    using PatternSizeEntrypoint = uint64_t (*)();

    void unload();

    std::filesystem::path module_path_;
    void *handle_{nullptr};
    PlanEntrypoint plan_{nullptr};
    PatternSizeEntrypoint pattern_size_{nullptr};
};

std::filesystem::path generated_plan_source_path();
std::filesystem::path generated_plan_module_path();
std::string generated_plan_module_target();
void build_generated_plan_module(const std::filesystem::path &build_dir);

} // namespace minigraph
