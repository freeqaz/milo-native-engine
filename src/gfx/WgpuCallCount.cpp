// Every wgpu buffer, texture, texture-view and bind-group create, counted and
// timed, whoever calls it (np-framecost4, docs section 40).
//
// The engine makes these from ~130 call sites, rb3's paths and imgui's backend
// among them, so counting at the sites would miss whichever one a later change
// adds. On Linux desktop the engine's link options wrap the C entry points
// instead (-Wl,--wrap=wgpuDeviceCreateBuffer and friends, native CMakeLists):
// every reference to wgpuDeviceCreateBuffer in the linked program, including
// the inline wrappers in webgpu_cpp.h, resolves to __wrap_wgpuDeviceCreateBuffer
// below, which times __real_wgpuDeviceCreateBuffer (Dawn's) into
// gfx/FrameCost.h. Elsewhere (macOS, the web) nothing is wrapped, this file
// compiles to nothing, and framecost::RawCallsCounted() is false.
//
// Bind groups are also counted at their sites (CreateCountedBindGroup, which
// every build has, for [GPUCOUNT] and the web); the wrapped count here is the
// check that no site bypasses it (RawBindGroupCreateCount).
#include "gfx/WgpuCallCount.h"

#include "gfx/FrameCost.h"

#include <chrono>

#ifdef MILO_WGPU_CALL_COUNT

#include <webgpu/webgpu.h>

namespace {
std::atomic<uint64_t> sRawBindGroups{0};

struct Timed {
    framecost::Kind kind;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Timed(framecost::Kind k) : kind(k) {}
    ~Timed() {
        framecost::Nanos(kind) += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
        framecost::Count(kind)++;
    }
};
} // namespace

extern "C" {
WGPUBuffer __real_wgpuDeviceCreateBuffer(WGPUDevice, WGPUBufferDescriptor const*);
WGPUTexture __real_wgpuDeviceCreateTexture(WGPUDevice, WGPUTextureDescriptor const*);
WGPUTextureView __real_wgpuTextureCreateView(WGPUTexture, WGPUTextureViewDescriptor const*);
WGPUBindGroup __real_wgpuDeviceCreateBindGroup(WGPUDevice, WGPUBindGroupDescriptor const*);

WGPUBuffer __wrap_wgpuDeviceCreateBuffer(WGPUDevice d, WGPUBufferDescriptor const* desc) {
    Timed t(framecost::kBuffer);
    return __real_wgpuDeviceCreateBuffer(d, desc);
}
WGPUTexture __wrap_wgpuDeviceCreateTexture(WGPUDevice d, WGPUTextureDescriptor const* desc) {
    Timed t(framecost::kGpuTexture);
    return __real_wgpuDeviceCreateTexture(d, desc);
}
WGPUTextureView __wrap_wgpuTextureCreateView(WGPUTexture tex, WGPUTextureViewDescriptor const* desc) {
    Timed t(framecost::kView);
    return __real_wgpuTextureCreateView(tex, desc);
}
WGPUBindGroup __wrap_wgpuDeviceCreateBindGroup(WGPUDevice d, WGPUBindGroupDescriptor const* desc) {
    sRawBindGroups++;
    return __real_wgpuDeviceCreateBindGroup(d, desc);
}
}

uint64_t RawBindGroupCreateCount() { return sRawBindGroups.load(); }

#else

uint64_t RawBindGroupCreateCount() { return 0; }

#endif

bool WgpuCallCountLinked() { return framecost::RawCallsCounted(); }
