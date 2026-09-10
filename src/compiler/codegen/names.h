#pragma once

#include <string>

namespace minigraph::codegen_names {
// Depth identifies a scheduled matching position, not an original query label.
inline std::string vertex(int depth) { return "v" + std::to_string(depth); }
inline std::string index(int depth) { return vertex(depth) + "_idx"; }
inline std::string adjacency(int depth) { return vertex(depth) + "_adj"; }
inline std::string bit_index(int depth) { return vertex(depth) + "_bit_idx"; }
inline std::string set_level(int depth) { return "SetLevel" + std::to_string(depth); }
inline std::string bit_level(int depth) { return "BitLevel" + std::to_string(depth); }
inline std::string bitmap(int id) { return "b" + std::to_string(id); }

inline constexpr const char *guide =
    "// Naming (D is matching depth; N is an IR set ID):\n"
    "// sN         : array-backed vertex set with IR set ID N\n"
    "// bN         : bitmap with IR set ID N (same ID as its array representation)\n"
    "// vD         : global vertex ID matched at depth D\n"
    "// vD_idx     : position in the prefix set iterated at depth D\n"
    "// vD_adj     : adjacency list or bitmap row of vD\n"
    "// vD_bit_idx : position of vD in the current bitmap universe\n"
    "// SetLevelD / BitLevelD : array / bitmap task at matching depth D\n\n";
} // namespace minigraph::codegen_names
