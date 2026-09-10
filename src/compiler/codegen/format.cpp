#include "compiler/codegen/format.h"
#include "compiler/compilation_profile.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#ifndef GRAPHMINI_CLANG_FORMAT
#define GRAPHMINI_CLANG_FORMAT ""
#endif
namespace minigraph {
namespace {
std::string quote(const std::string &value) {
#ifdef _WIN32
    if (value.find_first_of("\"%\r\n") != std::string::npos)
        throw std::runtime_error("Unsupported formatter path");
    return "\"" + value + "\"";
#else
    std::string result = "'";
    for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
#endif
}
void warn() {
    static std::atomic_flag warned = ATOMIC_FLAG_INIT;
    if (!warned.test_and_set())
        std::cerr << "GraphMini: clang-format unavailable or failed; using unformatted generated code.\n";
}
struct Temporary {
    std::filesystem::path directory;
    ~Temporary() {
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
};
}
std::string format_generated_cpp(const std::string &source) {
    const char *enabled = std::getenv("GRAPHMINI_FORMAT_CODE");
    if (enabled && std::string(enabled) == "0") return source;
    CompilationStage formatting("clang_format");
    if (std::string(GRAPHMINI_CLANG_FORMAT).empty()) { warn(); return source; }
    try {
        Temporary temporary;
        static std::atomic<uint64_t> sequence{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("graphmini-format-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(candidate)) { temporary.directory = candidate; break; }
        }
        if (temporary.directory.empty()) throw std::runtime_error("Cannot create formatter directory");
        const auto file = temporary.directory / "plan.cpp";
        {
            std::ofstream output(file);
            output << source;
            output.close();
            if (!output) throw std::runtime_error("Cannot write formatter input");
        }
        const auto command = quote(GRAPHMINI_CLANG_FORMAT) + " -i --style=" +
            quote("{BasedOnStyle: LLVM, IndentWidth: 4, ColumnLimit: 0, SortIncludes: false, ReflowComments: false}") +
            " " + quote(file.string());
        if (std::system(command.c_str()) != 0) throw std::runtime_error("Formatter failed");
        std::ifstream input(file);
        std::string formatted((std::istreambuf_iterator<char>(input)), {});
        if (input.bad() || formatted.empty()) throw std::runtime_error("Cannot read formatter output");
        return formatted;
    } catch (const std::exception &) { warn(); return source; }
}
}
