#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;
class PipelineManager;
class RndMat;
namespace Hmx { struct Rect; struct Color; }

// The factor retail's Xbox DrawRect gives a rect drawn with `mat` through the
// colour overload (kDrawRectShader, shapes with rndshape::kRectXboxDrawRectShader):
// the material colour, alpha included, with rgb premultiplied by alpha for
// kPreMultAlpha (NgMat::SetRegularShaderConst's c0). A prelit material
// multiplies the colour argument by it; any other material draws it alone
// (the `standard` VS writes `mul o2, c1, c0` with c1 = white). Matched:
// DxRnd::DrawRect (both overloads) and RndShaderDrawRect::Select, 100%.
Hmx::Color DrawRectMaterialColor(RndMat* mat);

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
