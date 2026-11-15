// Every bind group the engine creates, counted.
//
// A CreateBindGroup is a validation pass and a descriptor allocation on
// desktop, and on the web a call out of wasm into the browser's WebGPU, so a
// bind group made per draw or per camera switch is per-frame work that no
// pipeline or texture counter sees. BindGroupCache's own creates= count only
// the groups that go through a cache. So every creation site calls
// CreateCountedBindGroup instead of wgpu::Device::CreateBindGroup:
// [GPUCOUNT] bgCreatesAll= counts them all, [GPUBG] names the ones created
// since the last [GPUCOUNT] line by descriptor label, and the frame-times CSV
// has each frame's count and time (bg_creates, bg_ms; gfx/FrameCost.h).
#pragma once

#include <webgpu/webgpu_cpp.h>

#include "gfx/FrameCost.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bindgroupcount {
inline std::atomic<uint64_t>& Counter() {
    static std::atomic<uint64_t> n{0};
    return n;
}
struct LabelCount {
    std::string label;
    uint64_t count = 0;
    double ms = 0.0;
};
inline std::mutex& RecentMutex() {
    static std::mutex m;
    return m;
}
// Creates since the last TakeRecentBindGroupLabels, by label (at most 64 labels).
inline std::vector<LabelCount>& Recent() {
    static std::vector<LabelCount> v;
    return v;
}
} // namespace bindgroupcount

inline wgpu::BindGroup CreateCountedBindGroup(const wgpu::Device& dev,
                                              const wgpu::BindGroupDescriptor* desc) {
    const auto t0 = std::chrono::steady_clock::now();
    wgpu::BindGroup group = dev.CreateBindGroup(desc);
    const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    bindgroupcount::Counter()++;
    framecost::Nanos(framecost::kBindGroup) += ns;
    framecost::Count(framecost::kBindGroup)++;
    const wgpu::StringView& l = desc->label;
    const size_t len = l.data ? (l.length == WGPU_STRLEN ? std::strlen(l.data) : l.length) : 0;
    std::lock_guard<std::mutex> lock(bindgroupcount::RecentMutex());
    auto& recent = bindgroupcount::Recent();
    for (bindgroupcount::LabelCount& r : recent) {
        if (r.label.size() == len && (len == 0 || std::memcmp(r.label.data(), l.data, len) == 0)) {
            r.count++;
            r.ms += (double)ns / 1e6;
            return group;
        }
    }
    if (recent.size() < 64) {
        bindgroupcount::LabelCount r;
        if (len) r.label.assign(l.data, len);  // empty = unlabelled
        r.count = 1;
        r.ms = (double)ns / 1e6;
        recent.push_back(std::move(r));
    }
    return group;
}

// Bind groups created so far, by every caller.
inline uint64_t GpuBindGroupCreateCount() { return bindgroupcount::Counter().load(); }

// The labels of the bind groups created since the last call, with counts, then cleared.
inline std::vector<bindgroupcount::LabelCount> TakeRecentBindGroupLabels() {
    std::lock_guard<std::mutex> lock(bindgroupcount::RecentMutex());
    std::vector<bindgroupcount::LabelCount> out;
    out.swap(bindgroupcount::Recent());
    return out;
}
