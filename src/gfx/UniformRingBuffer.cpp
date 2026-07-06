// Shared bump-allocated uniform ring buffer (gfx-core, rndobj-free).
// Method bodies extracted verbatim from the DC3 backend (Rnd_Wgpu.cpp). See W1.5.

#include "gfx/UniformRingBuffer.h"

#include <cstdio>

void UniformRingBuffer::Init(wgpu::Device& device, uint32_t capacity, const char* label) {
    mDevice = device;
    mLabel = label ? label : "UniformRing";
    wgpu::BufferDescriptor desc{};
    desc.label = mLabel;
    desc.size = capacity;
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mBuffer = device.CreateBuffer(&desc);
    mCapacity = capacity;
    mOffset = 0;
}

void UniformRingBuffer::Grow(wgpu::Device& device) {
    uint32_t newCapacity = mCapacity * 2;
#ifdef DEBUG_LOGS
    fprintf(stderr, "UniformRingBuffer: growing %s %u -> %u bytes\n", mLabel, mCapacity, newCapacity);
#endif

    wgpu::BufferDescriptor desc{};
    desc.label = mLabel;
    desc.size = newCapacity;
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    // Old buffer stays alive until GPU is done with current frame (ref-counted by Dawn)
    mBuffer = device.CreateBuffer(&desc);
    mCapacity = newCapacity;
    mOffset = 0;
}

uint32_t UniformRingBuffer::Write(wgpu::Queue& queue, const void* data, uint32_t size) {
    uint32_t alignedSize = (size + kAlignment - 1) & ~(kAlignment - 1);
    if (mOffset + alignedSize > mCapacity) {
        Grow(mDevice);
    }
    uint32_t offset = mOffset;
    queue.WriteBuffer(mBuffer, offset, data, size);
    mOffset += alignedSize;
    return offset;
}
