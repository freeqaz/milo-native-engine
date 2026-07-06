// Shared bump-allocated uniform ring buffer (gfx-core, rndobj-free).
// Extracted verbatim from the DC3 backend (Rnd_Wgpu.h) so both the DC3 and RB3
// WebGPU backends compile a single definition. See W1.5.

#pragma once

#include <webgpu/webgpu_cpp.h>

// ============================================================================
// Uniform ring buffer — writes to different offsets per draw call
// ============================================================================

class UniformRingBuffer {
public:
    void Init(wgpu::Device& device, uint32_t capacity, const char* label = nullptr);
    void Reset() { mOffset = 0; }
    void Release() { mBuffer = nullptr; mDevice = nullptr; }

    // Write data at next aligned offset, return the offset used
    uint32_t Write(wgpu::Queue& queue, const void* data, uint32_t size);

    wgpu::Buffer& Buffer() { return mBuffer; }
    uint32_t Capacity() const { return mCapacity; }

private:
    void Grow(wgpu::Device& device);

    static constexpr uint32_t kAlignment = 256; // minUniformBufferOffsetAlignment
    wgpu::Device mDevice;
    wgpu::Buffer mBuffer;
    uint32_t mCapacity = 0;
    uint32_t mOffset = 0;
    const char* mLabel = "UniformRing";
};
