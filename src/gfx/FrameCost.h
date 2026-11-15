// Where a frame's work goes: time spent creating render pipelines, creating
// GPU textures from bitmaps, uploading mesh geometry, creating bind groups,
// buffers, textures and texture views, acquiring and presenting the surface,
// and reading files, summed per kind on every thread that does it, with a
// count per kind. The frame-times log (MILO_FRAME_TIMES) writes each frame's
// share (pipe_ms / tex_ms / mesh_ms / bg_ms ...), so a hitch can be
// attributed without a profiler (milo-native-engine
// docs/native/dc3-backend-for-rb3-wii.md sections 39 and 40).
//
// kBuffer / kGpuTexture / kView are the raw wgpu creates (every caller), and
// are counted only where the build can see every call (gfx/WgpuCallCount.cpp:
// Linux desktop links with --wrap); elsewhere they stay 0 and
// framecost::RawCallsCounted() is false. kTexture is the bitmap-to-GPU
// conversion (Tex_Wgpu), which includes its own raw creates.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace framecost {

enum Kind {
    kPipeline = 0,
    kTexture,
    kMesh,
    kBindGroup,
    kBuffer,
    kGpuTexture,
    kView,
    kAcquire,
    kPresent,
    kFileRead,
    kKinds
};

inline std::atomic<uint64_t>& Nanos(Kind k) {
    static std::atomic<uint64_t> sNanos[kKinds];
    return sNanos[k];
}

inline std::atomic<uint64_t>& Count(Kind k) {
    static std::atomic<uint64_t> sCount[kKinds];
    return sCount[k];
}

// True when kBuffer / kGpuTexture / kView see every wgpu create (the build
// wraps the C entry points; gfx/WgpuCallCount.cpp).
#ifdef MILO_WGPU_CALL_COUNT
inline constexpr bool RawCallsCounted() { return true; }
#else
inline constexpr bool RawCallsCounted() { return false; }
#endif

// Adds the time from construction to destruction to `k`.
class Scope {
public:
    explicit Scope(Kind k) : mKind(k), mStart(std::chrono::steady_clock::now()) {}
    ~Scope() {
        Nanos(mKind) += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - mStart)
                            .count();
        Count(mKind)++;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind mKind;
    std::chrono::steady_clock::time_point mStart;
};

} // namespace framecost
