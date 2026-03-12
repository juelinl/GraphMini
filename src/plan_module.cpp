#include "plan_module.h"

#include "backend/backend.h"
#include "common.h"
#include "configure.h"

#include <cstdlib>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace minigraph {

namespace {

std::string library_prefix() {
#ifdef _WIN32
    return "";
#else
    return "lib";
#endif
}

std::string library_suffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

template <typename SymbolT>
SymbolT load_symbol(void *handle, const char *symbol_name, const std::filesystem::path &module_path) {
#ifdef _WIN32
    FARPROC symbol = GetProcAddress(static_cast<HMODULE>(handle), symbol_name);
    CHECK(symbol != nullptr) << "Failed to resolve symbol '" << symbol_name << "' in " << module_path;
    return reinterpret_cast<SymbolT>(symbol);
#else
    dlerror();
    auto *symbol = reinterpret_cast<SymbolT>(dlsym(handle, symbol_name));
    const char *error = dlerror();
    CHECK(error == nullptr) << "Failed to resolve symbol '" << symbol_name << "' in " << module_path << ": " << error;
    return symbol;
#endif
}

} // namespace

LoadedPlanModule::LoadedPlanModule(const std::filesystem::path &module_path) : module_path_(module_path) {
#ifdef _WIN32
    const auto module_path_w = module_path_.wstring();
    handle_ = static_cast<void *>(LoadLibraryW(module_path_w.c_str()));
    CHECK(handle_ != nullptr) << "Failed to load plan module " << module_path_;
#else
    handle_ = dlopen(module_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK(handle_ != nullptr) << "Failed to load plan module " << module_path_ << ": " << dlerror();
#endif
    plan_ = load_symbol<PlanEntrypoint>(handle_, kGeneratedPlanEntrypoint, module_path_);
    pattern_size_ = load_symbol<PatternSizeEntrypoint>(handle_, kGeneratedPlanPatternSizeEntrypoint, module_path_);
}

LoadedPlanModule::~LoadedPlanModule() {
    unload();
}

LoadedPlanModule::LoadedPlanModule(LoadedPlanModule &&other) noexcept
        : module_path_(std::move(other.module_path_)),
          handle_(other.handle_),
          plan_(other.plan_),
          pattern_size_(other.pattern_size_) {
    other.handle_ = nullptr;
    other.plan_ = nullptr;
    other.pattern_size_ = nullptr;
}

LoadedPlanModule &LoadedPlanModule::operator=(LoadedPlanModule &&other) noexcept {
    if (this != &other) {
        unload();
        module_path_ = std::move(other.module_path_);
        handle_ = other.handle_;
        plan_ = other.plan_;
        pattern_size_ = other.pattern_size_;
        other.handle_ = nullptr;
        other.plan_ = nullptr;
        other.pattern_size_ = nullptr;
    }
    return *this;
}

void LoadedPlanModule::unload() {
    if (handle_ != nullptr) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }
    plan_ = nullptr;
    pattern_size_ = nullptr;
}

uint64_t LoadedPlanModule::pattern_size() const {
    CHECK(pattern_size_ != nullptr) << "Plan module is not loaded";
    return pattern_size_();
}

void LoadedPlanModule::run(const Graph *graph, Context &ctx) const {
    CHECK(plan_ != nullptr) << "Plan module is not loaded";
    plan_(graph, &ctx);
}

std::filesystem::path generated_plan_source_path() {
    std::filesystem::path source_path(PROJECT_SOURCE_DIR);
    source_path /= "src";
    source_path /= "codegen_output";
    source_path /= "plan.cpp";
    return source_path;
}

std::filesystem::path generated_plan_module_path() {
    std::filesystem::path module_path(CMAKE_LIBRARY_OUTPUT_DIRECTORY);
    module_path /= library_prefix() + generated_plan_module_target() + library_suffix();
    return module_path;
}

std::string generated_plan_module_target() {
    return "plan_module";
}

void build_generated_plan_module(const std::filesystem::path &build_dir) {
    std::ostringstream command;
    command << "cmake --build " << build_dir << " --target " << generated_plan_module_target();
    const int status = std::system(command.str().c_str());
    CHECK(status == 0) << "Failed to build generated plan module with command: " << command.str();
}

} // namespace minigraph
