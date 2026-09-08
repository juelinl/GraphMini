#pragma once

#include <cstdint>
#include <limits>

namespace minigraph {
using IdType = std::uint32_t;
inline constexpr IdType INVALID_ID = std::numeric_limits<IdType>::max();
} // namespace minigraph
