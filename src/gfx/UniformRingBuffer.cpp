// Shared bump-allocated uniform ring buffer (gfx-core, rndobj-free).
// Method bodies extracted verbatim from the DC3 backend (Rnd_Wgpu.cpp). See W1.5.

#include "gfx/UniformRingBuffer.h"

#include <cstdio>
#include <cstring>

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
    mFlushed = 0;
    if (mStaged) mShadow.assign(mCapacity, 0);
}

void UniformRingBuffer::SetStaged(bool staged) {
    mStaged = staged;
    if (mStaged && mShadow.size() != mCapacity) mShadow.assign(mCapacity, 0);
    if (!mStaged) {
        mShadow.clear();
        mShadow.shrink_to_fit();
    }
}

void UniformRingBuffer::Flush(wgpu::Queue& queue) {
    if (!mStaged || !mBuffer || mOffset <= mFlushed) return;
    // mFlushed and mOffset are both multiples of kAlignment, so the size is a
    // multiple of 4 as WriteBuffer requires.
    const uint32_t bytes = mOffset - mFlushed;
    queue.WriteBuffer(mBuffer, mFlushed, mShadow.data() + mFlushed, bytes);
    ++mQueueWrites;
    mQueueBytes += bytes;
    mFlushed = mOffset;
}

void UniformRingBuffer::Grow(wgpu::Queue& queue) {
    // What the old buffer was given must reach it before the switch: commands
    // already recorded this frame read the old buffer.
    Flush(queue);
    uint32_t newCapacity = mCapacity * 2;
#ifdef DEBUG_LOGS
    fprintf(stderr, "UniformRingBuffer: growing %s %u -> %u bytes\n", mLabel, mCapacity, newCapacity);
#endif

    wgpu::BufferDescriptor desc{};
    desc.label = mLabel;
    desc.size = newCapacity;
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    // Old buffer stays alive until GPU is done with current frame (ref-counted by Dawn)
    mBuffer = mDevice.CreateBuffer(&desc);
    mCapacity = newCapacity;
    mOffset = 0;
    mFlushed = 0;
    ++mGrows;
    if (mStaged) mShadow.assign(mCapacity, 0);
}

uint32_t UniformRingBuffer::Write(wgpu::Queue& queue, const void* data, uint32_t size) {
    uint32_t alignedSize = (size + kAlignment - 1) & ~(kAlignment - 1);
    if (mOffset + alignedSize > mCapacity) {
        Grow(queue);
    }
    uint32_t offset = mOffset;
    if (mStaged) {
        std::memcpy(mShadow.data() + offset, data, size);
    } else {
        queue.WriteBuffer(mBuffer, offset, data, size);
        ++mQueueWrites;
        mQueueBytes += size;
    }
    mOffset += alignedSize;
    return offset;
}
