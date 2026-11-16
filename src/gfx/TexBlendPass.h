// TexBlendPass — retail RndTexBlender::DrawShowing's draws on WebGPU.
//
// One render pass over the blender's output texture (no depth):
//   1. the base map over the whole target, blend off (retail: DrawRect with
//      the work material, kBlendSrc, alpha 1, white);
//   2. each layer's mesh with retail's unwrapuv shader: the vertex goes to
//      its UV (u, v) -> clip (2u - 1, 1 - 2v), so v = 0 is texel row 0, the
//      row the head material samples at v = 0; the pixel writes
//      vec4(tex(uv).rgb, alpha) (retail `tfetch2D r0.xyz1` then
//      `mul oC0, r0, r2`, with r2 the work material's white colour and the
//      layer alpha), blend SrcAlpha / InvSrcAlpha, clamp sampling
//      (SetupMaterial's kTexWrapClamp), no culling.
// Each draw has its own parameter block: a slot of one ring buffer, bound at
// a dynamic offset, so a layer's bind group depends only on its texture. With
// a BindGroupCache the groups are made once and reused frame to frame (no
// per-frame CreateBindGroup); without one each Record makes its own.
//
// Rndobj-free: the caller (WgpuRnd) resolves textures and mesh buffers.
#pragma once
#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

class GpuDevice;
class BindGroupCache;

class TexBlendPass {
public:
    struct Layer {
        wgpu::Buffer vertexBuffer;   // GpuVertex / GpuVertexSkinned stream
        uint32_t vertexStride = 0;   // 64 or 88; the UV is at byte 40 in both
        uint64_t vertexBytes = 0;
        wgpu::Buffer indexBuffer;    // uint16 triangle list
        uint32_t indexCount = 0;
        wgpu::TextureView tex;
        float alpha = 1.0f;
    };

    // Records one pass into `target` (w x h, `format`). `base` may be null:
    // the target is then loaded and only the layers draw (retail leaves the
    // target as it was). Returns false, recording nothing, on a missing
    // target or a pipeline failure.
    bool Record(wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
                wgpu::TextureFormat format, uint32_t w, uint32_t h,
                const wgpu::TextureView& base, const Layer* layers, size_t count,
                GpuDevice& gpu, BindGroupCache* cache = nullptr);
    // Creates `format`'s pipelines now (a load-time warm, PLAN H-104);
    // returns false on a pipeline failure.
    bool Warm(wgpu::TextureFormat format, GpuDevice& gpu) { return EnsurePipelines(format, gpu) != nullptr; }
    // A new command encoder: the slots the last one used have been submitted.
    void BeginFrame() { mFrameUsed = 0; }
    void Terminate();

    // Byte offset of the UV in both mesh vertex layouts (gfx/VertexFormats.h).
    static constexpr uint32_t kUVOffset = 40;

private:
    struct Pipelines {
        wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
        wgpu::RenderPipeline base;
        wgpu::RenderPipeline unwrap64;   // static vertex stride
        wgpu::RenderPipeline unwrap88;   // skinned vertex stride
    };
    Pipelines* EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu);

    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mBgl;
    wgpu::PipelineLayout mLayout;
    wgpu::Sampler mSampler;
    std::vector<Pipelines> mPipelines;
    // Parameter ring: kParamStride-byte slots, used in order and wrapped. A
    // slot is rewritten only after the command buffers recorded since its
    // last write have been submitted (Queue::WriteBuffer is ordered with
    // Submit): the slots one frame (BeginFrame to BeginFrame) uses never
    // overlap, and a frame that needs more than the ring holds grows it.
    wgpu::Buffer mParams;
    uint32_t mParamSlots = 0;
    uint32_t mParamNext = 0;
    uint32_t mFrameUsed = 0;
};
