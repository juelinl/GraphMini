#include <atomic>
import tbb;

int main() {
    std::atomic<int> sum{0};
    tbb::parallel_for(0, 100, [&](int i) { sum.fetch_add(i, std::memory_order_relaxed); });
    tbb::cache_aligned_allocator<int> aligned;
    int* a = aligned.allocate(1);
    aligned.deallocate(a, 1);
    tbb::scalable_allocator<int> scalable;
    int* b = scalable.allocate(1);
    scalable.deallocate(b, 1);
    return sum.load() != 4950 || TBB_runtime_interface_version() < 12190;
}
