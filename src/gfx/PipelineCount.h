// Every render pipeline the engine creates, counted.
//
// A pipeline compile is the most expensive thing a frame can do by accident
// (WGSL -> Tint -> SPIR-V -> vkCreateGraphicsPipelines; tens of ms on a
// handheld). [GPUCOUNT] pipelines= is only PipelineManager's cache size, and
// most passes own their pipelines outside it -- the depth of field, spot beam
// and point-test pipelines that used to compile together on the first song
// frame never showed up there. So every creation site calls
// CreateCountedRenderPipeline instead of wgpu::Device::CreateRenderPipeline:
// [GPUCOUNT] pipelineCreates= counts them all, and [GPUPIPE] names the ones
// created since the last [GPUCOUNT] line (the descriptor's label) with the
// time the create call took on this thread.
#pragma once

#include <webgpu/webgpu_cpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pipelinecount {
inline std::atomic<uint64_t>& Counter() {
    static std::atomic<uint64_t> n{0};
    return n;
}
inline std::mutex& RecentMutex() {
    static std::mutex m;
    return m;
}
inline std::vector<std::string>& Recent() {
    static std::vector<std::string> v;
    return v;
}
} // namespace pipelinecount

inline wgpu::RenderPipeline CreateCountedRenderPipeline(const wgpu::Device& dev,
                                                        const wgpu::RenderPipelineDescriptor* desc) {
    const auto t0 = std::chrono::steady_clock::now();
    wgpu::RenderPipeline pipe = dev.CreateRenderPipeline(desc);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    pipelinecount::Counter()++;
    std::lock_guard<std::mutex> lock(pipelinecount::RecentMutex());
    auto& recent = pipelinecount::Recent();
    if (recent.size() < 64) {
        auto str = [](const wgpu::StringView& v) {
            return v.data ? std::string(v.data, v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length)
                          : std::string();
        };
        // An unlabelled pipeline is named by its entry points instead.
        std::string name = str(desc->label);
        if (name.empty()) {
            name = "vs:" + str(desc->vertex.entryPoint);
            if (desc->fragment) name += "/fs:" + str(desc->fragment->entryPoint);
        }
        char t[32];
        snprintf(t, sizeof(t), " (%.1f ms)", ms);
        recent.emplace_back(std::move(name) + t);
    }
    return pipe;
}

// Render pipelines created so far, by every caller.
inline uint64_t GpuPipelineCreateCount() { return pipelinecount::Counter().load(); }

// The labels of the pipelines created since the last call (at most 64), then cleared.
inline std::vector<std::string> TakeRecentPipelineLabels() {
    std::lock_guard<std::mutex> lock(pipelinecount::RecentMutex());
    std::vector<std::string> out;
    out.swap(pipelinecount::Recent());
    return out;
}
