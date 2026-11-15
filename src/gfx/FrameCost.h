// Where a frame's first-use work goes: time spent creating render pipelines,
// creating GPU textures from bitmaps, and uploading mesh geometry, summed per
// kind on every thread that does it. The frame-times log (MILO_FRAME_TIMES)
// writes each frame's share as pipe_ms / tex_ms / mesh_ms, so a hitch can be
// attributed without a profiler (milo-native-engine
// docs/native/dc3-backend-for-rb3-wii.md section 39).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace framecost {

enum Kind { kPipeline = 0, kTexture, kMesh, kKinds };

inline std::atomic<uint64_t>& Nanos(Kind k) {
    static std::atomic<uint64_t> sNanos[kKinds];
    return sNanos[k];
}

// Adds the time from construction to destruction to `k`.
class Scope {
public:
    explicit Scope(Kind k) : mKind(k), mStart(std::chrono::steady_clock::now()) {}
    ~Scope() {
        Nanos(mKind) += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - mStart)
                            .count();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind mKind;
    std::chrono::steady_clock::time_point mStart;
};

} // namespace framecost
