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
    wgpu::Buffer m2dVertexBuffer;
    bool m2dPipelineReady = false;
};
