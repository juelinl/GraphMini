#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace minigraph {
// Specialize once per universe, outside the matching loops. Zero selects the
// dynamic-word implementation; all branches must return the same type.
template <class Function>
decltype(auto) dispatch_bitmap_words(std::size_t bits, Function&& function) {
    if (bits <= 64)
        return std::forward<Function>(function)(std::integral_constant<std::size_t, 1>{});
    if (bits <= 128)
        return std::forward<Function>(function)(std::integral_constant<std::size_t, 2>{});
    if (bits <= 256)
        return std::forward<Function>(function)(std::integral_constant<std::size_t, 4>{});
    if (bits <= 512)
        return std::forward<Function>(function)(std::integral_constant<std::size_t, 8>{});
    return std::forward<Function>(function)(std::integral_constant<std::size_t, 0>{});
}
} // namespace minigraph
