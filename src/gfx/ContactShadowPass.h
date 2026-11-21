// ContactShadowPass — soft contact shadows under DC3's crowd (enhanced look,
// np-look32; platform/ContactShadowHook.h has the why).
//
// One quad per shadow, already in clip space (the backend transforms its four
// corners with the view-projection current when the shadow was queued). The
// fragment shader draws an analytic radial falloff, no texture:
//
//   g = (exp(-3 r^2) - exp(-3)) / (1 - exp(-3)),  r = 1 at the quad's edge
//   factor = 1 - kStrength * g   (display-referred; kStrength 0.45 at the centre)
//
// written with a multiply blend (colour = dst * factor), depth test LessEqual
// against the open pass's depth, no depth write, no alpha write (DC3's frame
// alpha is the retail post chain's bloom mask). An sRGB target blends in linear
// light, so the factor is raised to 2.2 there to darken the stored, encoded
// value by about the display-referred amount.
//
// Rndobj-free.
#pragma once
#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

class GpuDevice;

class ContactShadowPass {
public:
    struct Vertex {
        float clip[4];  // clip-space position
        float uv[2];    // place on the ellipse: (+-1, +-1) at the quad's corners
    };
    static constexpr float kStrength = 0.45f;   // darkening at the centre, display-referred
    static constexpr int kMaxShadowsPerFrame = 1024;

    void Terminate();
    // Starts a frame: the vertex buffer is reused from its start.
    void BeginFrame() { mUsed = 0; }
    // Records `verts` (6 per shadow, two triangles) into `pass`, whose colour
    // target has `format` and `samples` and which has a Depth24PlusStencil8
    // attachment. Returns the shadows drawn (0 when none fit this frame).
    int Draw(GpuDevice& gpu, wgpu::RenderPassEncoder& pass, wgpu::TextureFormat format,
             uint32_t samples, const std::vector<Vertex>& verts);
    // Creates the pipeline for that target ahead of the first draw.
    void Warm(GpuDevice& gpu, wgpu::TextureFormat format, uint32_t samples);
    // True once the pipeline for that target exists (a draw into it compiles nothing).
    bool Ready(wgpu::TextureFormat format, uint32_t samples) const;

private:
    wgpu::RenderPipeline PipelineFor(GpuDevice& gpu, wgpu::TextureFormat format, uint32_t samples);

    struct Entry {
        wgpu::TextureFormat format;
        uint32_t samples;
        wgpu::RenderPipeline pipe;
    };
    wgpu::ShaderModule mShader;
    wgpu::PipelineLayout mLayout;
    std::vector<Entry> mPipes;
    wgpu::Buffer mVB;
    uint64_t mUsed = 0;  // bytes of mVB written this frame
};
