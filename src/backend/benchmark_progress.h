#pragma once

// Opt-in, process-surviving snapshots for externally bounded TBB benchmarks.
// Readers must wait for plan return or process exit; this is NOT a live API.
#include "backend/backend.h"
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#ifndef GRAPHMINI_USE_TBB_MODULE
#include <oneapi/tbb/task_arena.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace minigraph {
class BenchmarkProgress {
    Context& ctx_;
    unsigned char* data_{nullptr};
    size_t bytes_{0};
public:
    BenchmarkProgress(Context& ctx, size_t vertices): ctx_(ctx) {
        const char* path = std::getenv("GRAPHMINI_PROGRESS_FILE");
        if (!path || !*path) return;
#if defined(__unix__) || defined(__APPLE__)
        const size_t slots = ctx.per_thread_result.size();
        bytes_ = 64 + 64 * slots + vertices;
        int fd = ::open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) throw std::runtime_error("Cannot create benchmark progress file");
        if (::ftruncate(fd, bytes_) != 0) {
            ::close(fd);
            throw std::runtime_error("Cannot size benchmark progress file");
        }
        void* mapping = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map benchmark progress file");
        data_ = static_cast<unsigned char*>(mapping);
        const uint64_t header[] = {1, slots, vertices,
                                  static_cast<uint64_t>(ctx.iep_redundency)};
        std::memcpy(data_, header, sizeof(header));
        for (size_t i = 0; i < slots; ++i)
            ctx.per_thread_result[i].progress_count = reinterpret_cast<long long*>(data_ + 64 + 64*i);
#else
        throw std::runtime_error("Benchmark progress requires a POSIX platform");
#endif
    }
    BenchmarkProgress(const BenchmarkProgress&) = delete;
    ~BenchmarkProgress() {
        if (!data_) return;
        for (auto& counter : ctx_.per_thread_result) {
            counter.progress_count = nullptr;
        }
#if defined(__unix__) || defined(__APPLE__)
        ::munmap(data_, bytes_);
#endif
    }
    volatile unsigned char* root(size_t vertex) {
        return data_ ? data_ + 64 + 64 * ctx_.per_thread_result.size() + vertex : nullptr;
    }
    void add_bitmap_matches(uint64_t count) {
        if (!data_ || !count) return;
        auto* slot = ctx_.per_thread_result.at(tbb::this_task_arena::current_thread_index()).progress_count;
        *slot = *slot + count;
    }
};

class BenchmarkRootProgress {
    volatile unsigned char* flag_;
    int exceptions_{std::uncaught_exceptions()};
public:
    explicit BenchmarkRootProgress(volatile unsigned char* flag): flag_(flag) {
        if (flag_) *flag_ = 1;
    }
    ~BenchmarkRootProgress() {
        if (flag_ && exceptions_ == std::uncaught_exceptions()) *flag_ = 2;
    }
};
}
