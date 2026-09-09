#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "backend.h"
#include "../bitgraph.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace minigraph::metal {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
struct Region { uint64_t offset; uint32_t degree, stride; };
struct Task { uint32_t region, vertex; };
struct Parameters { uint32_t tasks, scratch_stride; };
struct Count { uint64_t matches, overflow; };
static_assert(sizeof(Region) == 16 && sizeof(Task) == 8 && sizeof(Parameters) == 8 && sizeof(Count) == 16);
std::runtime_error failure(const char *message, NSError *error = nil) {
    return std::runtime_error(std::string(message) + (error ? ": " + std::string(error.localizedDescription.UTF8String) : ""));
}
void add_count(uint64_t &sum, uint64_t value) {
    if (value > UINT64_MAX-sum) throw std::overflow_error("Clique count exceeds uint64");
    sum += value;
}
}
struct Backend::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    id<MTLComputePipelineState> pipelines[9];
    Impl() {
        device = MTLCreateSystemDefaultDevice();
        if (!device || !device.hasUnifiedMemory || ![device supportsFamily:MTLGPUFamilyApple7])
            throw failure("Metal backend requires an Apple7-or-newer unified-memory GPU");
        queue = [device newCommandQueue];
        NSError *error = nil;
        library = [device newLibraryWithURL:[NSURL fileURLWithPath:@GRAPHMINI_METALLIB] error:&error];
        if (!library || !queue) throw failure("Cannot initialize Metal", error);
    }
    id<MTLComputePipelineState> pipeline(unsigned k) {
        if (!pipelines[k]) {
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            uint32_t size = k;
            [values setConstantValue:&size type:MTLDataTypeUInt atIndex:0];
            NSError *error = nil;
            id<MTLFunction> function = [library newFunctionWithName:@"clique_count" constantValues:values error:&error];
            if (!function) throw failure("Cannot specialize Metal clique kernel", error);
            pipelines[k] = [device newComputePipelineStateWithFunction:function error:&error];
            if (!pipelines[k]) throw failure("Cannot create Metal pipeline", error);
        }
        return pipelines[k];
    }
    id<MTLBuffer> buffer(size_t bytes, const void *data = nullptr) {
        if (bytes > device.maxBufferLength) throw failure("Metal buffer exceeds device limit");
        id<MTLBuffer> result = [device newBufferWithLength:std::max<size_t>(bytes,8) options:MTLResourceStorageModeShared];
        if (!result) throw failure("Metal buffer allocation failed");
        if (data && bytes) std::memcpy(result.contents, data, bytes);
        return result;
    }
};
Backend::Backend() { @autoreleasepool { impl_ = std::make_unique<Impl>(); } }
Backend::~Backend() = default;
std::string Backend::device_name() const { return impl_->device.name.UTF8String; }
Result Backend::count_cliques(const Adjacency &g, const Options &o) {
    @autoreleasepool {
        if (o.clique_size < 3 || o.clique_size > 8) throw std::invalid_argument("Metal supports cliques K3 through K8 only");
        if (o.batch_bytes < 1024 || o.scratch_bytes < 1024 || !o.max_tasks || o.max_tasks > UINT32_MAX)
            throw std::invalid_argument("Invalid Metal batch limits");
        if (g.size() > UINT32_MAX) throw std::length_error("Graph exceeds uint32 IDs");
        // Pipeline specialization is session setup, outside query timing.
        id<MTLComputePipelineState> pipeline = impl_->pipeline(o.clique_size);
        Result result;
        const auto start = Clock::now();
        for (size_t v = 0; v < g.size(); ++v) {
            internal::require_sorted_ids(g[v].data(),g[v].size());
            for (uint32_t u : g[v])
                if (u >= g.size() || u == v || !std::binary_search(g[u].begin(),g[u].end(),uint32_t(v)))
                    throw std::invalid_argument("Metal requires sorted simple undirected adjacency");
        }
        result.validation_seconds = elapsed(start);
        std::vector<uint64_t> words;
        std::vector<Region> regions;
        std::vector<Task> tasks;
        uint32_t max_stride = 0;
        auto flush = [&] {
            if (tasks.empty()) return;
            @autoreleasepool {
                const auto begin = Clock::now();
                const size_t scratch_size = tasks.size()*size_t(max_stride)*(o.clique_size-2)*8;
                auto matrix = impl_->buffer(words.size()*8, words.data());
                auto metadata = impl_->buffer(regions.size()*sizeof(Region),regions.data());
                auto work = impl_->buffer(tasks.size()*sizeof(Task),tasks.data());
                auto scratch = impl_->buffer(scratch_size);
                auto counts = impl_->buffer(tasks.size()*sizeof(Count));
                result.peak_buffer_bytes = std::max(result.peak_buffer_bytes,
                    words.size()*8+regions.size()*sizeof(Region)+tasks.size()*(sizeof(Task)+sizeof(Count))+scratch_size);
                Parameters parameters{uint32_t(tasks.size()),max_stride};
                id<MTLCommandBuffer> command = [impl_->queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                if (!command || !encoder) throw failure("Cannot create Metal commands");
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:matrix offset:0 atIndex:0];
                [encoder setBuffer:metadata offset:0 atIndex:1];
                [encoder setBuffer:work offset:0 atIndex:2];
                [encoder setBuffer:scratch offset:0 atIndex:3];
                [encoder setBuffer:counts offset:0 atIndex:4];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5];
                const NSUInteger width = pipeline.threadExecutionWidth;
                [encoder dispatchThreads:MTLSizeMake(tasks.size(),1,1) threadsPerThreadgroup:MTLSizeMake(width,1,1)];
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                if (command.status != MTLCommandBufferStatusCompleted) throw failure("Metal execution failed",command.error);
                result.gpu_seconds += command.GPUEndTime-command.GPUStartTime;
                const auto *values = static_cast<const Count *>(counts.contents);
                for (size_t i = 0; i < tasks.size(); ++i) {
                    if (values[i].overflow) throw std::overflow_error("Metal task count exceeds uint64");
                    add_count(result.matches,values[i].matches);
                }
                result.dispatch_wait_seconds += elapsed(begin);
                result.tasks += tasks.size();
                ++result.batches;
                tasks.clear(); regions.clear(); words.clear(); max_stride = 0;
            }
        };
        for (uint32_t root = 0; root < g.size(); ++root) {
            const auto &neighbors = g[root];
            const size_t prefix = std::lower_bound(neighbors.begin(),neighbors.end(),root)-neighbors.begin();
            if (prefix < o.clique_size-1) continue;
            const size_t stride = bit_ops::word_count(neighbors.size());
            const size_t bytes = neighbors.size()*stride*8;
            if (bytes > o.batch_bytes || stride*8*(o.clique_size-2) > o.scratch_bytes)
                throw std::length_error("Root BitGraph exceeds batch budget; raise explicit Metal limits");
            const auto begin = Clock::now();
            BitGraph graph(NeighborhoodUniverse(root,neighbors),neighbors,
                           [&](uint32_t v) -> const std::vector<uint32_t> & { return g[v]; });
            result.construction_seconds += elapsed(begin);
            ++result.regions;
            size_t next = o.clique_size-2;
            while (next < prefix) {
                if (words.size()*8+bytes > o.batch_bytes) flush();
                const size_t new_stride = std::max<size_t>(max_stride,stride);
                const size_t capacity = std::min(o.max_tasks,o.scratch_bytes/(new_stride*8*(o.clique_size-2)));
                if (tasks.size() >= capacity) { flush(); continue; }
                const size_t take = std::min(prefix-next,capacity-tasks.size());
                Region region{words.size(),uint32_t(neighbors.size()),uint32_t(stride)};
                const uint32_t region_id = uint32_t(regions.size());
                regions.push_back(region);
                const auto packing = Clock::now();
                for (size_t row = 0; row < neighbors.size(); ++row)
                    words.insert(words.end(),graph.row_data_at(row),graph.row_data_at(row)+stride);
                result.construction_seconds += elapsed(packing);
                for (size_t i = 0; i < take; ++i) tasks.push_back({region_id,uint32_t(next+i)});
                max_stride = uint32_t(new_stride);
                next += take;
                if (next < prefix) flush();
            }
        }
        flush();
        result.total_seconds = elapsed(start);
        return result;
    }
}
} // namespace minigraph::metal
