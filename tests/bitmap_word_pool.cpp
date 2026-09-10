#include "backend/bitmap_words.h"
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace minigraph::internal;
void require(bool value) { if (!value) throw std::logic_error("Bitmap pool contract"); }
int main() {
    auto &pool = BitmapWordPool::local();
    pool.trim();
    for (size_t words : {9, 16, 17, 24, 25, 63, 64, 65, 513}) {
        const auto capacity = BitmapWordPool::capacity_for(words);
        require(capacity % 8 == 0 && capacity >= words && capacity - words < 8);
        auto buffer = pool.acquire(capacity);
        require(reinterpret_cast<uintptr_t>(buffer.get()) % 64 == 0);
        pool.release(std::move(buffer), capacity);
        auto recycled = pool.acquire(capacity);
        require(reinterpret_cast<uintptr_t>(recycled.get()) % 64 == 0);
    }
    pool.trim();
    uint64_t *released;
    {
        BitmapWords<8> first(9, 99);
        released = first.data();
        BitmapWords<8> nested(9, 77);
        require(nested.data() != first.data());
        auto copy = first;
        require(copy.data() != first.data() && copy[8] == 99);
        copy[0] = 0;
        require(first[0] == 99);
    }
    {
        BitmapWords<8> reused(15);
        require(reused.data() == released); // Same 512-bit-multiple capacity bin.
        for (auto word : reused) require(word == 0); // No stale word leakage.
        auto moved = std::move(reused);
        require(moved.data() == released);
    }
    std::promise<BitmapWords<8>> promise;
    auto future = promise.get_future();
    std::thread producer([&] { promise.set_value(BitmapWords<8>(513, 42)); });
    producer.join(); // Allocator thread exits before the buffer is released.
    { auto survivor = future.get(); require(survivor[512] == 42); }
    {
        std::vector<BitmapWords<8>> active;
        for (size_t i = 0; i < 32; ++i) active.emplace_back(8193, i);
        for (size_t i = 0; i < active.size(); ++i) require(active[i][8192] == i);
    }
    require(pool.cached_bytes() <= BitmapWordPool::cache_limit_bytes);
    pool.trim();
    require(pool.cached_bytes() == 0);
    {
        BitmapWords<8> oversized(BitmapWordPool::cache_limit_bytes / 8 + 1, 5);
        require(oversized[0] == 5);
    }
    require(pool.cached_bytes() == 0);
    // TLS owner constructed before the cache: owner destruction happens last.
    std::thread teardown([] {
        static thread_local std::unique_ptr<BitmapWords<8>> owner;
        owner = std::make_unique<BitmapWords<8>>(9, 1);
    });
    teardown.join();
}
