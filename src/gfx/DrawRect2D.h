#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;
class PipelineManager;
class RndMat;
namespace Hmx { struct Rect; struct Color; }

class DrawRect2D {
public:
    void Init(GpuDevice& gpu);
    void Draw(wgpu::RenderPassEncoder& pass, const Hmx::Rect& rect, RndMat* mat,
              const Hmx::Color& color, const Hmx::Color* topRight, const Hmx::Color* botLeft,
              GpuDevice& gpu, PipelineManager& pipelines,
              wgpu::TextureView& whiteTexView, wgpu::Sampler& defaultSampler);
    void Terminate();

private:
    void EnsurePipeline(GpuDevice& gpu);

    wgpu::ShaderModule m2dShader;
    wgpu::BindGroupLayout m2dBindGroupLayout;
    wgpu::PipelineLayout m2dPipelineLayout;
    // Per-frame arena of 6-vertex rects: Queue::WriteBuffer runs ahead of the
    // whole frame's command buffer, so every rect needs its own slot.
    wgpu::Buffer m2dVertexBuffer;
    int m2dVBCapacity = 0;   // rects
    int m2dVBUsed = 0;       // rects written this frame
    int m2dVBFrame = -1;     // WgpuRnd::FrameID() the arena belongs to
    bool m2dPipelineReady = false;
};
