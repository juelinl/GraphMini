#pragma once
#include <string>
namespace minigraph {
// Best-effort presentation pass. GRAPHMINI_FORMAT_CODE=0 preserves raw output.
std::string format_generated_cpp(const std::string &source);
}
