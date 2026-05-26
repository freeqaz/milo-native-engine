#include "gfx/DrawRect2D.h"
#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "math/Geo.h"
#include "math/Color.h"
#include "rndobj/Mat.h"
#include "rndobj/Rnd.h"
#include "obj/Object.h"

#include <cstring>

static const char* k2DShaderSource = R"WGSL(
struct Vertex2D {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

struct VSOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

@vertex fn vs_2d(in: Vertex2D) -> VSOut {
    var out: VSOut;
    out.pos = vec4f(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@group(0) @binding(0) var rectTex: texture_2d<f32>;
@group(0) @binding(1) var rectSampler: sampler;

@fragment fn fs_2d(in: VSOut) -> @location(0) vec4f {
    let texColor = textureSample(rectTex, rectSampler, in.uv);
    return texColor * in.color;
}

@fragment fn fs_2d_notex(in: VSOut) -> @location(0) vec4f {
    return in.color;
}
)WGSL";

struct Vertex2D {
    float pos[2];
    float uv[2];
    float color[4];
};

void DrawRect2D::Init(GpuDevice& gpu) {
    EnsurePipeline(gpu);
}

void DrawRect2D::EnsurePipeline(GpuDevice& gpu) {
    if (m2dPipelineReady) return;

    auto& dev = gpu.Device();

    wgpu::ShaderSourceWGSL wgslSource;
    wgslSource.code = k2DShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgslSource;
    m2dShader = dev.CreateShaderModule(&smDesc);

    wgpu::BindGroupLayoutEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 2;
    bglDesc.entries = entries;
    m2dBindGroupLayout = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &m2dBindGroupLayout;
    m2dPipelineLayout = dev.CreatePipelineLayout(&plDesc);

    wgpu::BufferDescriptor vbDesc{};
    vbDesc.size = 6 * sizeof(Vertex2D);
    vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    m2dVertexBuffer = dev.CreateBuffer(&vbDesc);

    m2dPipelineReady = true;
}

void DrawRect2D::Draw(wgpu::RenderPassEncoder& pass, const Hmx::Rect& rect, RndMat* mat,
                      const Hmx::Color& color, const Hmx::Color* topRight, const Hmx::Color* botLeft,
                      GpuDevice& gpu, PipelineManager& pipelines,
                      wgpu::TextureView& whiteTexView, wgpu::Sampler& defaultSampler) {
    EnsurePipeline(gpu);

    auto& dev = gpu.Device();

    // Use the Rnd virtual resolution (e.g. 768×432 widescreen) for coordinate conversion,
    // NOT the GPU framebuffer size (1280×720). The engine generates DrawRect coordinates in
    // Rnd pixel space (Width()×Height()), so we must map [0,Width] → NDC [-1,1].
    float w = (float)TheRnd.Width();
    float h = (float)TheRnd.Height();
    if (w <= 0 || h <= 0) return;

    float x0 = rect.x / w * 2.0f - 1.0f;
    float y0 = 1.0f - rect.y / h * 2.0f;
    float x1 = (rect.x + rect.w) / w * 2.0f - 1.0f;
    float y1 = 1.0f - (rect.y + rect.h) / h * 2.0f;

    float cTL[4] = { color.red, color.green, color.blue, color.alpha };
    float cTR[4], cBL[4], cBR[4];
    if (topRight) {
        cTR[0] = topRight->red; cTR[1] = topRight->green;
        cTR[2] = topRight->blue; cTR[3] = topRight->alpha;
    } else {
        memcpy(cTR, cTL, sizeof(cTL));
    }
    if (botLeft) {
        cBL[0] = botLeft->red; cBL[1] = botLeft->green;
        cBL[2] = botLeft->blue; cBL[3] = botLeft->alpha;
    } else {
        memcpy(cBL, cTL, sizeof(cTL));
    }
    cBR[0] = (cTR[0] + cBL[0]) * 0.5f;
    cBR[1] = (cTR[1] + cBL[1]) * 0.5f;
    cBR[2] = (cTR[2] + cBL[2]) * 0.5f;
    cBR[3] = (cTR[3] + cBL[3]) * 0.5f;

    Vertex2D verts[6] = {
        {{x0, y0}, {0, 0}, {cTL[0], cTL[1], cTL[2], cTL[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y1}, {1, 1}, {cBR[0], cBR[1], cBR[2], cBR[3]}},
    };

    gpu.Queue().WriteBuffer(m2dVertexBuffer, 0, verts, sizeof(verts));

    bool hasTex = false;
    wgpu::TextureView texView;
    if (mat && mat->GetDiffuseTex()) {
        texView = GetGpuTexView(mat->GetDiffuseTex());
        if (texView) hasTex = true;
    }
    if (!hasTex) texView = whiteTexView;

    WgpuBlend blend = WgpuBlend::SrcAlpha;
    if (mat) blend = (WgpuBlend)mat->GetBlend();

    wgpu::BlendState bs = pipelines.MapBlend(blend);
    WgpuRnd* rnd = gWgpuRnd;
    if (!rnd) return;

    wgpu::ColorTargetState ct{};
    ct.format = rnd->CurrentTargetFormat();
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = m2dShader;
    frag.entryPoint = hasTex ? "fs_2d" : "fs_2d_notex";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[3] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x2; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 8; attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 16; attrs[2].shaderLocation = 2;

    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(Vertex2D);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 3;
    vbl.attributes = attrs;

    wgpu::DepthStencilState ds{};
    if (rnd->CurrentPassHasDepth()) {
        ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::Always;
    }

    wgpu::RenderPipelineDescriptor pipeDesc{};
    pipeDesc.layout = m2dPipelineLayout;
    pipeDesc.vertex.module = m2dShader;
    pipeDesc.vertex.entryPoint = "vs_2d";
    pipeDesc.vertex.bufferCount = 1;
    pipeDesc.vertex.buffers = &vbl;
    pipeDesc.fragment = &frag;
    pipeDesc.depthStencil = rnd->CurrentPassHasDepth() ? &ds : nullptr;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeDesc.multisample.count = rnd->CurrentSampleCount();

    wgpu::RenderPipeline pipe = dev.CreateRenderPipeline(&pipeDesc);

    wgpu::BindGroupEntry bgEntries[2] = {};
    bgEntries[0].binding = 0;
    bgEntries[0].textureView = texView;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = defaultSampler;

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = m2dBindGroupLayout;
    bgDesc.entryCount = 2;
    bgDesc.entries = bgEntries;
    wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.SetVertexBuffer(0, m2dVertexBuffer, 0, sizeof(verts));
    pass.Draw(6);
}

void DrawRect2D::Terminate() {
    m2dShader = nullptr;
    m2dBindGroupLayout = nullptr;
    m2dPipelineLayout = nullptr;
    m2dVertexBuffer = nullptr;
    m2dPipelineReady = false;
}
