// Shared bump-allocated uniform ring buffer (gfx-core, rndobj-free).
// Extracted verbatim from the DC3 backend (Rnd_Wgpu.h) so both the DC3 and RB3
// WebGPU backends compile a single definition. See W1.5.

#pragma once

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

// ============================================================================
// Uniform ring buffer — writes to different offsets per draw call
// ============================================================================
//
// Staged mode (SetStaged(true), PLAN H-31): Write() copies into a CPU shadow
// of the buffer instead of calling Queue::WriteBuffer per draw, and Flush()
// uploads everything written since the last flush with ONE WriteBuffer. The
// owner must Flush() before submitting any command buffer that reads the ring
// (WgpuRnd does it right before its frame Submit). The bytes the GPU sees at a
// given offset are exactly the bytes passed to Write() for it; only the number
// of queue writes changes. A Grow() flushes what the old buffer was given
// before switching. Unstaged (the default) is the old one-write-per-call ring.

class UniformRingBuffer {
public:
    void Init(wgpu::Device& device, uint32_t capacity, const char* label = nullptr);
    void Reset() { mOffset = 0; mFlushed = 0; }
    void Release() { mBuffer = nullptr; mDevice = nullptr; mShadow.clear(); mShadow.shrink_to_fit(); }

    // Write data at next aligned offset, return the offset used
    uint32_t Write(wgpu::Queue& queue, const void* data, uint32_t size);

    // Staged mode only: upload [last flush, current offset) in one write.
    // No-op when nothing is pending or the ring is unstaged.
    void Flush(wgpu::Queue& queue);
    void SetStaged(bool staged);
    bool Staged() const { return mStaged; }

    wgpu::Buffer& Buffer() { return mBuffer; }
    uint32_t Capacity() const { return mCapacity; }
    uint32_t Used() const { return mOffset; }
    // Counters: Queue::WriteBuffer calls and bytes this ring has issued.
    uint64_t QueueWrites() const { return mQueueWrites; }
    uint64_t QueueBytes() const { return mQueueBytes; }
    uint32_t Grows() const { return mGrows; }

    static constexpr uint32_t kAlignment = 256; // minUniformBufferOffsetAlignment

private:
    void Grow(wgpu::Queue& queue);

    wgpu::Device mDevice;
    wgpu::Buffer mBuffer;
    uint32_t mCapacity = 0;
    uint32_t mOffset = 0;
    uint32_t mFlushed = 0;          // staged: shadow bytes [0, mFlushed) are uploaded
    bool mStaged = false;
    std::vector<uint8_t> mShadow;   // staged: CPU copy of the buffer
    uint64_t mQueueWrites = 0;
    uint64_t mQueueBytes = 0;
    uint32_t mGrows = 0;
    const char* mLabel = "UniformRing";
};
