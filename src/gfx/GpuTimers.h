#pragma once

// GPU time per render pass (WebGPU timestamp queries).
//
// MILO_GPU_TIMERS=1 turns it on. GpuDevice then asks the adapter for the
// `timestamp-query` feature (and, natively, turns Dawn's timestamp_quantization
// toggle off so passes shorter than ~0.1 ms are measurable). Every render pass
// whose begin site calls MILO_GPU_TIME_PASS(desc) gets a begin/end timestamp
// pair; the pass's descriptor label is its name, and a pass begun without one
// shows up as "unnamed@<file>:<line>". At the end of a frame the frame's
// queries are resolved into a buffer and copied into one of a small ring of
// readback buffers, which is mapped asynchronously and read a few frames later
// when the map has completed. Nothing ever waits on the GPU: when every
// readback buffer is still in flight the frame simply is not timed (counted as
// "skipped").
//
// Passes with the same name within a frame aggregate (sum of ms, count). Over
// a ~1 s window the per-frame values are averaged; the window's result is
// printed as [GPUTIME] lines on stderr and kept as the latest snapshot
// (Latest() / LatestJson(), safe from any thread).
//
// Off (unset, or the adapter has no timestamp-query, or the web build): no
// query set or buffer is created, MILO_GPU_TIME_PASS is one load of a bool,
// EndFrame returns at once. The reason it is off is logged once when it was
// asked for.

#include <webgpu/webgpu_cpp.h>

#include <mutex>
#include <string>
#include <vector>

class GpuTimers {
public:
    static GpuTimers& Get();

    // MILO_GPU_TIMERS is set (truthy). Read once.
    static bool Requested();

    // GpuDevice, after device creation. `featureEnabled`: the device was
    // created with FeatureName::TimestampQuery. When false, `whyNot` is logged
    // as "GPU timers unavailable: <whyNot>" (only if Requested()).
    // `quantizationOff`: 1 the device confirms timestamp_quantization is off,
    // 0 it is on (passes under ~0.1 ms read 0 on adapters where Dawn applies
    // it), -1 unknown (the toggle was disabled but cannot be read back).
    void Init(const wgpu::Device& device, const wgpu::Instance& instance, bool featureEnabled,
              const char* whyNot, int quantizationOff);
    // Requested but cannot run (no device path reached Init, the web build).
    void NoteUnavailable(const char* why);
    void Shutdown();

    // True only when timers were requested AND the device has the feature.
    static bool Active() { return sActive; }

    // Fill desc.timestampWrites for this pass (or clear it when this frame is
    // not being timed). Call immediately before BeginRenderPass(&desc).
    void InstrumentPass(wgpu::RenderPassDescriptor& desc, const char* file, int line);

    // After the frame's last submit (and present): resolve this frame's
    // queries, start the readback, collect any earlier readback that has
    // completed. Never blocks.
    void EndFrame(const wgpu::Queue& queue);

    struct PassStat {
        std::string name;
        double meanMs = 0.0;   // mean over the window's timed frames (sum within a frame)
        double maxMs = 0.0;    // max per-frame value
        double meanCount = 0.0;  // passes of this name per timed frame
    };
    struct Snapshot {
        bool active = false;
        std::string unavailableReason;  // non-empty when requested but off
        int quantizationOff = -1;  // as Init's argument
        int frames = 0;        // timed frames in the window
        int skipped = 0;       // frames not timed (every readback buffer in flight / overflow)
        double windowSeconds = 0.0;
        double sumMeanMs = 0.0, sumMaxMs = 0.0;    // sum of all passes per frame
        double spanMeanMs = 0.0, spanMaxMs = 0.0;  // first begin to last end per frame
        long long windowIndex = 0;  // increments each published window
        std::vector<PassStat> passes;  // sorted by meanMs, largest first
    };
    Snapshot Latest() const;
    // {"active":...,"frames":..,"passes":[{"name":..,"ms":..,"max":..,"n":..}]}
    std::string LatestJson() const;

    // Test hook: a window shorter than 1 s (seconds; <= 0 restores 1 s).
    void SetWindowSeconds(double s);

    // The render targets the frame is drawn through (np-outres): every
    // texture with RenderAttachment usage GpuDevice::CreateTexture makes, one
    // "[GPUTARGET] <label> <w>x<h> samples=<n>" line on stderr the first time
    // a label is made at a size, so a run shows which targets follow the
    // frame and which keep a size of their own. Requested() only; cheap (the
    // seen set is small: targets are made at a resize, not per frame).
    void NoteTarget(const wgpu::TextureDescriptor& desc);

private:
    GpuTimers() = default;
    void Collect();
    void AccumulateFrame(int slot);
    void PublishWindow(double now);

    static bool sActive;

    struct Impl;
    Impl* mImpl = nullptr;
    mutable std::mutex mSnapMutex;
    Snapshot mSnap;
    double mWindowSeconds = 1.0;
    std::mutex mTargetMutex;
    std::vector<std::string> mTargetsSeen;
};

// At a pass begin site, just before encoder.BeginRenderPass(&desc).
#define MILO_GPU_TIME_PASS(desc)                                                  \
    do {                                                                          \
        if (GpuTimers::Active()) GpuTimers::Get().InstrumentPass((desc), __FILE__, __LINE__); \
    } while (0)
