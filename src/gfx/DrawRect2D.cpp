#include "gfx/DrawRect2D.h"
#include "gfx/BindGroupCount.h"
#include "gfx/GpuDevice.h"
#include "gfx/PipelineCount.h"
#include "gfx/PipelineManager.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "math/Geo.h"
#include "math/Color.h"
#include "rndobj/Mat.h"
#include "rndobj/Rnd.h"
#include "rndobj/Tex.h"
#include "platform/rndshape/RndShape.h"
#include "obj/Object.h"

#include <cmath>
#include <cstring>

static const char* k2DShaderSource = R"WGSL(
struct Vertex2D {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
    @location(3) mask: f32,
};

struct VSOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
    @location(2) @interpolate(flat) mask: f32,
};

@vertex fn vs_2d(in: Vertex2D) -> VSOut {
    var out: VSOut;
    out.pos = vec4f(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.mask = in.mask;
    return out;
}

@group(0) @binding(0) var rectTex: texture_2d<f32>;
@group(0) @binding(1) var rectSampler: sampler;

// The pseudo-HDR bloom mask (standard.ps ShaderOptions bit 22): a mask > 0
// writes alpha = luma(rgb) * mask, as a mesh with the same material does. The
// scale is the frame's post-proc's, so it rides in the vertices (the same on
// all six) rather than in the pipeline: a pipeline constant made every
// post-proc a new pipeline, compiled at the draw.
fn bloomMask(c: vec4f, mask: f32) -> vec4f {
    if (mask > 0.0) {
        return vec4f(c.rgb, clamp(dot(c.rgb, vec3f(0.3, 0.59, 0.11)) * mask, 0.0, 1.0));
    }
    return c;
}

@fragment fn fs_2d(in: VSOut) -> @location(0) vec4f {
    let texColor = textureSample(rectTex, rectSampler, in.uv);
    return bloomMask(texColor * in.color, in.mask);
}

@fragment fn fs_2d_notex(in: VSOut) -> @location(0) vec4f {
    return bloomMask(in.color, in.mask);
}
)WGSL";

struct Vertex2D {
    float pos[2];
    float uv[2];
    float color[4];
    float mask;  // bloom mask scale, the same on all six (k2DShaderSource)
};

Hmx::Color DrawRectMaterialColor(RndMat* mat) {
    Hmx::Color k = mat->GetColor();
    if ((int)mat->GetBlend() == 7) { // kPreMultAlpha
        k.red *= k.alpha;
        k.green *= k.alpha;
        k.blue *= k.alpha;
    }
    return k;
}

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

    m2dVertexBuffer = nullptr;
    m2dVBCapacity = 0;
    m2dVBUsed = 0;
    m2dVBFrame = -1;

    m2dPipelineReady = true;
}

// The pipeline a rect drawn with `mat` (null: DrawRectMat's SrcAlpha) asks for
// in a pass: the draw's, or the load-time warm's (Warm).
// The bloom mask scale a rect drawn with `mat` writes in this pass (0: none).
static float RectMaskScale(RndMat* mat, bool hasDepth, bool toTexture) {
    if (!mat || !rndshape::RetailBloomMaskActive() || !hasDepth || toTexture) return 0.0f;
    // DC3's scale is negative (luma of the pre-clamp colour, which a rect
    // does not have: its colour is the clamped one, so the magnitude is used).
    // RndShaderDrawRect::CalcShaderOpts ignores the environ's fade-out.
    return std::fabs(rndshape::BloomMaskScale(rndshape::Mat(mat), false));
}

wgpu::RenderPipeline DrawRect2D::PipelineFor(GpuDevice& gpu, PipelineManager& pipelines,
                                             RndMat* mat, bool hasTex, bool maskWrite,
                                             wgpu::TextureFormat format, bool hasDepth,
                                             bool toTexture, uint32_t samples) {
    return PipelineForBlend(gpu, pipelines, mat, mat ? (WgpuBlend)mat->GetBlend() : WgpuBlend::SrcAlpha,
                            hasTex, maskWrite, format, hasDepth, toTexture, samples);
}

wgpu::RenderPipeline DrawRect2D::PipelineForBlend(GpuDevice& gpu, PipelineManager& pipelines,
                                                  RndMat* mat, WgpuBlend blend, bool hasTex,
                                                  bool maskWrite, wgpu::TextureFormat format,
                                                  bool hasDepth, bool toTexture, uint32_t samples) {
    EnsurePipeline(gpu);
    wgpu::BlendState bs = pipelines.MapBlend(blend);

    wgpu::ColorTargetState ct{};
    ct.format = format;
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;
    // RB3's retail post chain reads the main frame's alpha as its bloom mask.
    // Retail draws a rect through RndShaderDrawRect (rb3-xenon Shader.cpp):
    // CalcShaderOpts sets pseudo-HDR when !Offscreen() && mat->AllowHDR(), and
    // SetColorWriteMask (0x824A5AF8) writes alpha only under pseudo-HDR,
    // Offscreen() or mat->mAlphaWrite. A null material is DrawRectMat
    // (SrcAlpha, so not AllowHDR). So in the main frame an AllowHDR material
    // writes the luma mask, an alpha-write one its alpha, and every other rect
    // leaves alpha alone, as meshes (Mesh_Wgpu) and particles (Part_Wgpu) do.
    // `maskWrite`: this draw writes the mask (RectMaskScale > 0; the scale
    // itself is in the vertices). The mask is the frame's (DC3: the retail
    // post chain on, rb3-xenon: never).
    if (rndshape::RetailBloomMaskActive() && hasDepth && !toTexture && !maskWrite &&
        !(mat && rndshape::Mat(mat)->GetAlphaWrite()))
        ct.writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                       wgpu::ColorWriteMask::Blue;

    wgpu::FragmentState frag{};
    frag.module = m2dShader;
    frag.entryPoint = hasTex ? "fs_2d" : "fs_2d_notex";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[4] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x2; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 8; attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 16; attrs[2].shaderLocation = 2;
    attrs[3].format = wgpu::VertexFormat::Float32; attrs[3].offset = 32; attrs[3].shaderLocation = 3;

    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(Vertex2D);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 4;
    vbl.attributes = attrs;

    wgpu::DepthStencilState ds{};
    if (hasDepth) {
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
    pipeDesc.depthStencil = hasDepth ? &ds : nullptr;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeDesc.multisample.count = samples;

    // Cached: see gfx/RenderPipelineCache.h (a per-rect CreateRenderPipeline
    // recompiled the shader every frame).
    return m2dPipelines.GetOrCreate(
        gpu.Device(),
        RenderPipelineCache::MakeKey(bs, ct.format, ct.writeMask, hasDepth,
                                     pipeDesc.multisample.count, 0.0f, hasTex ? 1u : 0u),
        pipeDesc);
}

int DrawRect2D::Warm(GpuDevice& gpu, PipelineManager& pipelines, RndMat* mat,
                     wgpu::TextureFormat format, bool hasDepth, bool toTexture,
                     uint32_t samples) {
    const uint64_t before = GpuPipelineCreateCount();
    // A rect samples its material's diffuse texture when it has one (Draw; one
    // whose bitmap never uploads draws untextured and compiles at the draw).
    const bool hasTex = mat && mat->GetDiffuseTex();
    PipelineFor(gpu, pipelines, mat, hasTex, false, format, hasDepth, toTexture, samples);
    // Whether the mask is written is the frame's (a current post-proc), not
    // the material's, so a material that allows it is warmed both ways.
    if (mat && rndshape::RetailBloomMaskActive() && hasDepth && !toTexture &&
        rndshape::AllowsBloomMask(rndshape::Mat(mat)))
        PipelineFor(gpu, pipelines, mat, hasTex, true, format, hasDepth, toTexture, samples);
    return (int)(GpuPipelineCreateCount() - before);
}

int DrawRect2D::Warm(GpuDevice& gpu, PipelineManager& pipelines, WgpuBlend blend, bool hasTex,
                     wgpu::TextureFormat format, bool toTexture, uint32_t samples) {
    const uint64_t before = GpuPipelineCreateCount();
    PipelineForBlend(gpu, pipelines, nullptr, blend, hasTex, false, format, false, toTexture, samples);
    return (int)(GpuPipelineCreateCount() - before);
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
    float w = (float)rndshape::TheRndRef().Width();
    float h = (float)rndshape::TheRndRef().Height();
    // Xbox shape (DC3 and rb3-xenon): into a texture target, the target's own
    // pixels (kRectTargetPixels).
    if (rndshape::kRectTargetPixels && gWgpuRnd) {
        if (RndTex* target = gWgpuRnd->ActiveTargetTex()) {
            w = (float)target->Width();
            h = (float)target->Height();
        }
    }
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
    if (rndshape::kRectModulatesMatColor && mat) {
        const Hmx::Color& mc = mat->GetColor();
        const float m[4] = { mc.red, mc.green, mc.blue, mc.alpha };
        for (int i = 0; i < 4; i++) {
            cTL[i] *= m[i]; cTR[i] *= m[i]; cBL[i] *= m[i]; cBR[i] *= m[i];
        }
    }

    // No mask in the overlay pass (WgpuRnd::CurrentPassIsOverlay), depth or not.
    const float mask = gWgpuRnd && !gWgpuRnd->CurrentPassIsOverlay()
                           ? RectMaskScale(mat, gWgpuRnd->CurrentPassHasDepth(),
                                           gWgpuRnd->ActiveTargetTex() != nullptr)
                           : 0.0f;
    Vertex2D verts[6] = {
        {{x0, y0}, {0, 0}, {cTL[0], cTL[1], cTL[2], cTL[3]}, mask},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}, mask},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}, mask},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}, mask},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}, mask},
        {{x1, y1}, {1, 1}, {cBR[0], cBR[1], cBR[2], cBR[3]}, mask},
    };

    // Each rect gets its own slot in this frame's arena. A single 6-vertex
    // buffer rewritten per call left every rect of the frame drawing at the
    // LAST rect's position: WriteBuffer executes at Submit, before any of the
    // frame's draws. A full arena is replaced by a larger one; draws already
    // recorded keep the old buffer alive.
    int frame = gWgpuRnd ? gWgpuRnd->FrameID() : 0;
    if (frame != m2dVBFrame) {
        m2dVBFrame = frame;
        m2dVBUsed = 0;
    }
    if (!m2dVertexBuffer || m2dVBUsed >= m2dVBCapacity) {
        int cap = m2dVBCapacity < 64 ? 64 : m2dVBCapacity * 2;
        wgpu::BufferDescriptor vbDesc{};
        vbDesc.size = (uint64_t)cap * sizeof(verts);
        vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        m2dVertexBuffer = dev.CreateBuffer(&vbDesc);
        m2dVBCapacity = cap;
        m2dVBUsed = 0;
    }
    uint64_t vbOffset = (uint64_t)m2dVBUsed * sizeof(verts);
    m2dVBUsed++;
    gpu.Queue().WriteBuffer(m2dVertexBuffer, vbOffset, verts, sizeof(verts));

    bool hasTex = false;
    wgpu::TextureView texView;
    if (RndTex* diffTex = mat ? mat->GetDiffuseTex() : nullptr) {
        // Upload on first use (see MaterialSetup's ResolveMap): a texture only a
        // 2D rect samples is otherwise never uploaded.
        diffTex->PresyncBitmap();
        texView = GetGpuTexView(diffTex);
        if (texView) hasTex = true;
    }
    if (!hasTex) texView = whiteTexView;

    WgpuRnd* rnd = gWgpuRnd;
    if (!rnd) return;
    wgpu::RenderPipeline pipe =
        PipelineFor(gpu, pipelines, mat, hasTex, mask > 0.0f, rnd->CurrentTargetFormat(),
                    rnd->CurrentPassHasDepth(), rnd->ActiveTargetTex() != nullptr,
                    rnd->CurrentSampleCount());

    wgpu::BindGroupEntry bgEntries[2] = {};
    bgEntries[0].binding = 0;
    bgEntries[0].textureView = texView;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = defaultSampler;

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = m2dBindGroupLayout;
    bgDesc.entryCount = 2;
    bgDesc.entries = bgEntries;
    // Same view + sampler is the same group: cached (PLAN H-30), dropped
    // with the texture through WgpuRnd::ForgetTextureView.
    wgpu::BindGroup bg = gWgpuRnd ? gWgpuRnd->BindGroups().GetOrCreate(dev, bgDesc)
                                  : CreateCountedBindGroup(dev, &bgDesc);

    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.SetVertexBuffer(0, m2dVertexBuffer, vbOffset, sizeof(verts));
    pass.Draw(6);
}

void DrawRect2D::Terminate() {
    m2dPipelines.Clear();
    m2dShader = nullptr;
    m2dBindGroupLayout = nullptr;
    m2dPipelineLayout = nullptr;
    m2dVertexBuffer = nullptr;
    m2dVBCapacity = 0;
    m2dVBUsed = 0;
    m2dVBFrame = -1;
    m2dPipelineReady = false;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* DrawRect2DWgslSource() { return k2DShaderSource; }
