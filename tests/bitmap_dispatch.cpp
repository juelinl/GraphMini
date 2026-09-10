#include "backend/bitmap_dispatch.h"
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

using minigraph::dispatch_bitmap_words;

void require(bool value) {
    if (!value) throw std::logic_error("Bitmap dispatch contract");
}

struct RvalueOnly {
    std::unique_ptr<int> value;
    template <class Tag> int operator()(Tag) && { return *value; }
};

int main() {
    const std::array<std::pair<std::size_t, std::size_t>, 15> cases{{
        {0, 1}, {1, 1}, {63, 1}, {64, 1}, {65, 2}, {127, 2}, {128, 2},
        {129, 4}, {255, 4}, {256, 4}, {257, 8}, {511, 8}, {512, 8},
        {513, 0}, {std::numeric_limits<std::size_t>::max(), 0}}};
    for (const auto& entry : cases) {
        const auto bits = entry.first;
        const auto expected = entry.second;
        int calls = 0;
        auto count = [&](auto tag) { ++calls; return decltype(tag)::value; };
        require(dispatch_bitmap_words(bits, count) == expected && calls == 1);

        auto execute = [&](auto tag) { ++calls; require(decltype(tag)::value == expected); };
        static_assert(std::is_void_v<decltype(dispatch_bitmap_words(bits, execute))>);
        dispatch_bitmap_words(bits, execute);
        require(calls == 2);

        // Preserve references and support non-copyable, rvalue-qualified callables.
        int value = 7;
        auto reference = [&](auto) -> int& { return value; };
        static_assert(std::is_same_v<decltype(dispatch_bitmap_words(bits, reference)), int&>);
        dispatch_bitmap_words(bits, reference) = 9;
        require(value == 9);
        require(dispatch_bitmap_words(bits, RvalueOnly{std::make_unique<int>(11)}) == 11);
    }
}
