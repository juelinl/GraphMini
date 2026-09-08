#pragma once
#include <chrono>
#include <map>
#include <string>

namespace minigraph {
struct CompilationProfile {
    std::map<std::string, double> seconds;
    bool cache_hit{false};
};
// Telemetry only: per-thread scoped capture does not affect compiler decisions.
inline thread_local CompilationProfile* active_compilation_profile = nullptr;
struct CompilationCapture {
    CompilationProfile* previous;
    explicit CompilationCapture(CompilationProfile& profile)
        : previous(active_compilation_profile) { active_compilation_profile = &profile; }
    ~CompilationCapture() { active_compilation_profile = previous; }
    CompilationCapture(const CompilationCapture&) = delete;
    CompilationCapture& operator=(const CompilationCapture&) = delete;
};
struct CompilationStage {
    using Clock = std::chrono::steady_clock;
    CompilationProfile* profile{active_compilation_profile};
    const char* name;
    Clock::time_point start;
    explicit CompilationStage(const char* stage) : name(stage), start(Clock::now()) {}
    ~CompilationStage() {
        if (profile) profile->seconds[name] += std::chrono::duration<double>(Clock::now() - start).count();
    }
    CompilationStage(const CompilationStage&) = delete;
    CompilationStage& operator=(const CompilationStage&) = delete;
};
} // namespace minigraph
