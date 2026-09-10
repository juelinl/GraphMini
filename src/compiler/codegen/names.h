#pragma once

#include <string>

namespace minigraph::codegen_names {
// Depth identifies a scheduled matching position, not an original query label.
inline std::string vertex(int depth) { return "v" + std::to_string(depth); }
inline std::string index(int depth) { return vertex(depth) + "_idx"; }
inline std::string adjacency(int depth) { return vertex(depth) + "_adj"; }
inline std::string bit_index(int depth) { return vertex(depth) + "_bit_idx"; }

inline constexpr const char *guide =
    "// Naming (D is matching depth; N is an IR set ID):\n"
    "// sN         : prefix set N (array or bitmap), not matching depth N\n"
    "// vD         : global vertex ID matched at depth D\n"
    "// vD_idx     : position in the prefix set iterated at depth D\n"
    "// vD_adj     : adjacency list or bitmap row of vD\n"
    "// vD_bit_idx : position of vD in the current bitmap universe\n\n";
} // namespace minigraph::codegen_names
