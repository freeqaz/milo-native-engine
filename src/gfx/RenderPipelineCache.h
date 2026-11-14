// Small keyed cache of render pipelines for draw paths that build their
// pipeline descriptor per draw (DrawRect2D, particles).
//
// Why: wgpu::Device::CreateRenderPipeline is not cheap even for a descriptor
// Dawn has seen before. Dawn's content cache only holds pipelines that are
// still alive, and a pipeline created as a local dies with the frame's command
// buffer, so the next frame compiles it again: WGSL -> Tint -> SPIR-V ->
// spirv-val -> vkCreateGraphicsPipelines, per rect and per particle system,
// every frame. Measured on DC3 gameplay (YMCA, headless, RTX 3090): 18% of the
// main thread. Holding the pipeline here makes the second draw a map lookup.
//
// The key is every input the call sites vary per draw; the descriptor fields
// they never vary (shader module, layout, vertex layout, topology) are fixed
// for the cache's owner and are not part of it. A different shader module
// (re-init after Terminate) must Clear() the cache.
#pragma once

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>

class RenderPipelineCache {
public:
    using Key = std::array<uint32_t, 13>;

    // The per-draw inputs: blend state, colour target format and write mask,
    // depth attachment present, sample count, the kMaskScale override
    // constant (bit pattern) and a caller-defined variant (e.g. entry point).
    static Key MakeKey(const wgpu::BlendState& blend, wgpu::TextureFormat format,
                       wgpu::ColorWriteMask writeMask, bool hasDepth, uint32_t sampleCount,
                       float maskScale, uint32_t variant) {
        uint32_t maskBits;
        std::memcpy(&maskBits, &maskScale, sizeof(maskBits));
        return Key{(uint32_t)blend.color.operation, (uint32_t)blend.color.srcFactor,
                   (uint32_t)blend.color.dstFactor, (uint32_t)blend.alpha.operation,
                   (uint32_t)blend.alpha.srcFactor, (uint32_t)blend.alpha.dstFactor,
                   (uint32_t)format, (uint32_t)writeMask, hasDepth ? 1u : 0u,
                   sampleCount, maskBits, variant, 0u};
    }

    // Returns the cached pipeline for `key`, or creates it from `desc`.
    wgpu::RenderPipeline GetOrCreate(wgpu::Device& dev, const Key& key,
                                     const wgpu::RenderPipelineDescriptor& desc) {
        auto it = mPipelines.find(key);
        if (it != mPipelines.end()) {
            ++mHits;
            return it->second;
        }
        // A bound, so a caller with an unexpectedly wide key space (a float
        // constant per material, say) costs memory no worse than this.
        if (mPipelines.size() >= kMaxEntries) mPipelines.clear();
        wgpu::RenderPipeline pipe = dev.CreateRenderPipeline(&desc);
        mPipelines.emplace(key, pipe);
        ++mCreates;
        return pipe;
    }

    void Clear() { mPipelines.clear(); }
    size_t Size() const { return mPipelines.size(); }
    uint64_t Creates() const { return mCreates; }
    uint64_t Hits() const { return mHits; }

    static constexpr size_t kMaxEntries = 256;

private:
    std::map<Key, wgpu::RenderPipeline> mPipelines;
    uint64_t mCreates = 0;
    uint64_t mHits = 0;
};
