#include "compiler/codegen/format.h"
#include <cstdlib>
#include <stdexcept>
int main() {
    const std::string raw = "int main(){\nreturn 0;\n}\n";
    auto formatted = minigraph::format_generated_cpp(raw);
#ifdef GRAPHMINI_TEST_FORMAT_AVAILABLE
    if (formatted == raw || formatted.find("    return 0;") == std::string::npos)
        throw std::runtime_error("Default generated-code formatting failed");
    if (minigraph::format_generated_cpp(formatted) != formatted)
        throw std::runtime_error("Formatting is not idempotent");
#endif
#ifdef _WIN32
    _putenv_s("GRAPHMINI_FORMAT_CODE", "0");
#else
    setenv("GRAPHMINI_FORMAT_CODE", "0", 1);
#endif
    if (minigraph::format_generated_cpp(raw) != raw)
        throw std::runtime_error("Raw-code opt out failed");
}
