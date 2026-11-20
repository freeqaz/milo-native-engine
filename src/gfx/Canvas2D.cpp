// gfx/Canvas2D.cpp -- the GPU half of the 2D canvas (contract: gfx/Canvas2D.h;
// CPU half: gfx/Canvas2DBatch.cpp). One instanced pipeline: a quad is one
// 128-byte instance, the vertex shader makes its six corners from the vertex
// index, and the fragment shader does the rounded-box signed distance, the
// ring, the soft edge, the corner-colour blend and the texture modes.
#include "gfx/Canvas2D.h"
#include "gfx/Canvas2DBatch.h"

#include "gfx/GpuDevice.h"
#include "gfx/PipelineCount.h"
#include "gfx/RenderPipelineCache.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Tex.h"

#include <algorithm>
#include <cstring>

static const char* kCanvas2DShaderSource = R"WGSL(
// One quad per instance (Canvas2DInstance, gfx/Canvas2DBatch.h).
struct CanvasInstance {
    @location(0) rect: vec4f,   // centre.xy, half size.xy -- viewport px, y down
    @location(1) uv: vec4f,     // u0 v0 (top-left), u1 v1 (bottom-right)
    @location(2) tl: vec4f,     // corner colours, straight alpha
    @location(3) tr: vec4f,
    @location(4) bl: vec4f,
    @location(5) br: vec4f,
    @location(6) shape: vec4f,  // radius, border, softness (device px), tex mode
    @location(7) xform: vec4f,  // 2 / viewport w, 2 / viewport h, padding px, -
};

struct CanvasVSOut {
    @builtin(position) pos: vec4f,
    @location(0) local: vec2f,                    // px from the rect's centre
    @location(1) t: vec2f,                        // 0..1 across the rect; beyond it in the padding
    @location(2) @interpolate(flat) half: vec2f,
    @location(3) @interpolate(flat) uv: vec4f,
    @location(4) @interpolate(flat) tl: vec4f,
    @location(5) @interpolate(flat) tr: vec4f,
    @location(6) @interpolate(flat) bl: vec4f,
    @location(7) @interpolate(flat) br: vec4f,
    @location(8) @interpolate(flat) shape: vec4f,
};

@vertex fn vs_canvas(@builtin(vertex_index) vi: u32, in: CanvasInstance) -> CanvasVSOut {
    // TL BL TR, TR BL BR (Canvas2DBuilder::CornerNdc mirrors this).
    var corners = array<vec2f, 6>(vec2f(-1.0, -1.0), vec2f(-1.0, 1.0), vec2f(1.0, -1.0),
                                  vec2f(1.0, -1.0), vec2f(-1.0, 1.0), vec2f(1.0, 1.0));
    // The geometry grows by the padding so the soft edge's outer half has
    // pixels to land on.
    let off = corners[vi] * (in.rect.zw + vec2f(in.xform.z));
    let p = in.rect.xy + off;
    var out: CanvasVSOut;
    out.pos = vec4f(p.x * in.xform.x - 1.0, 1.0 - p.y * in.xform.y, 0.0, 1.0);
    out.local = off;
    out.t = off / in.rect.zw * 0.5 + vec2f(0.5);
    out.half = in.rect.zw;
    out.uv = in.uv;
    out.tl = in.tl;
    out.tr = in.tr;
    out.bl = in.bl;
    out.br = in.br;
    out.shape = in.shape;
    return out;
}

@group(0) @binding(0) var canvasTex: texture_2d<f32>;
@group(0) @binding(1) var canvasSampler: sampler;

// Signed distance to a box of half size `b` with corner radius `r`: negative
// inside, in the units of `p` (device pixels here).
fn sdRoundBox(p: vec2f, b: vec2f, r: f32) -> f32 {
    let q = abs(p) - b + vec2f(r);
    return length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

// 1 inside (d < 0), 0 outside, falling across `soft` px centred on the edge:
// linear at one pixel (a box filter: an edge on a pixel boundary stays exact),
// eased when wider (shadows, glows). soft == 0 is a hard step.
fn edgeCoverage(d: f32, soft: f32) -> f32 {
    if (soft <= 0.0) {
        return select(0.0, 1.0, d <= 0.0);
    }
    let c = clamp(0.5 - d / soft, 0.0, 1.0);
    let eased = c * c * (3.0 - 2.0 * c);
    return mix(c, eased, clamp(soft - 1.0, 0.0, 1.0));
}

@fragment fn fs_canvas(in: CanvasVSOut) -> @location(0) vec4f {
    // Bilinear over the four corners, held at the edge in the padding.
    let t = clamp(in.t, vec2f(0.0), vec2f(1.0));
    var color = mix(mix(in.tl, in.tr, t.x), mix(in.bl, in.br, t.x), t.y);

    // Sample inside the quad's own UV rectangle: clamped to its outermost
    // texel centres, so an atlas cell never filters in its neighbour.
    let halfTexel = vec2f(0.5) / vec2f(textureDimensions(canvasTex));
    let uvA = in.uv.xy;
    let uvB = in.uv.zw;
    let mid = (uvA + uvB) * 0.5;
    let lo = min(min(uvA, uvB) + halfTexel, mid);
    let hi = max(max(uvA, uvB) - halfTexel, mid);
    let texel = textureSample(canvasTex, canvasSampler, clamp(mix(uvA, uvB, in.t), lo, hi));

    let mode = in.shape.w;
    if (mode > 0.5) {
        // Coverage: one channel is alpha, the colour is the quad's.
        var cov = texel.a;
        if (mode < 1.5) {
            cov = texel.r;
        } else if (mode < 2.5) {
            cov = texel.g;
        } else if (mode < 3.5) {
            cov = texel.b;
        }
        color = vec4f(color.rgb, color.a * cov);
    } else {
        color = color * texel;
    }

    let d = sdRoundBox(in.local, in.half, in.shape.x);
    var a = edgeCoverage(d, in.shape.z);
    if (in.shape.y > 0.0) {
        // A ring: also outside the same shape shrunk by the border width.
        a = a * edgeCoverage(-(d + in.shape.y), in.shape.z);
    }
    return vec4f(color.rgb, color.a * a);
}
)WGSL";

struct Canvas2D::Impl {
    Canvas2DBuilder builder;
    wgpu::ShaderModule shader;
    wgpu::BindGroupLayout bindGroupLayout;
    wgpu::PipelineLayout pipelineLayout;
    wgpu::Sampler sampler;
    RenderPipelineCache pipelines;
    // Per-frame arena of instances: Queue::WriteBuffer runs ahead of the whole
    // frame's command buffer, so every flush of a frame needs its own range.
    wgpu::Buffer arena;
    uint32_t arenaCapacity = 0; // instances
    uint32_t arenaUsed = 0;
    int arenaFrame = -1;        // WgpuRnd::FrameID() the arena's contents belong to
    int statsFrame = -1;
    bool ready = false;

    void EnsureResources(GpuDevice& gpu);
    wgpu::RenderPipeline PipelineFor(GpuDevice& gpu, wgpu::TextureFormat format, bool hasDepth,
                                     bool colourOnly, uint32_t samples);
};

namespace {

// Whether a canvas draw in this pass leaves the target's alpha alone: the
// world pass of a frame whose alpha is the bloom mask (DrawRect2D's rule for a
// rect with no material).
bool PassKeepsAlpha(bool hasDepth, bool toTexture) {
    return rndshape::RetailBloomMaskActive() && hasDepth && !toTexture;
}

Canvas2DTarget ReadTarget(WgpuRnd& rnd) {
    Canvas2DTarget t;
    float vp[4];
    rnd.PassViewportPixels(vp);
    t.vpX = vp[0];
    t.vpY = vp[1];
    t.vpW = vp[2];
    t.vpH = vp[3];
    t.targetW = (int)rnd.CurrentTargetWidth();
    t.targetH = (int)rnd.CurrentTargetHeight();
    int sc[4];
    rnd.PassScissor(sc);
    t.baseClip = {sc[0], sc[1], sc[2], sc[3]};
    return t;
}

} // namespace

void Canvas2D::Impl::EnsureResources(GpuDevice& gpu) {
    if (ready) return;
    auto& dev = gpu.Device();

    wgpu::ShaderSourceWGSL wgslSource;
    wgslSource.code = kCanvas2DShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgslSource;
    shader = dev.CreateShaderModule(&smDesc);

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
    bindGroupLayout = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout;
    pipelineLayout = dev.CreatePipelineLayout(&plDesc);

    // Bilinear, mips when the texture has them, no wrap (the shader clamps to
    // the quad's UV rectangle before this could matter).
    SamplerDesc sd{};
    sd.addressU = wgpu::AddressMode::ClampToEdge;
    sd.addressV = wgpu::AddressMode::ClampToEdge;
    sampler = gpu.GetSampler(sd);

    arena = nullptr;
    arenaCapacity = 0;
    arenaUsed = 0;
    arenaFrame = -1;
    ready = true;
}

wgpu::RenderPipeline Canvas2D::Impl::PipelineFor(GpuDevice& gpu, wgpu::TextureFormat format,
                                                 bool hasDepth, bool colourOnly,
                                                 uint32_t samples) {
    EnsureResources(gpu);

    // Normal alpha from straight-alpha colours.
    wgpu::BlendState bs{};
    bs.color.operation = wgpu::BlendOperation::Add;
    bs.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    bs.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    bs.alpha.operation = wgpu::BlendOperation::Add;
    bs.alpha.srcFactor = wgpu::BlendFactor::One;
    bs.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;

    wgpu::ColorTargetState ct{};
    ct.format = format;
    ct.blend = &bs;
    ct.writeMask = colourOnly ? (wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                                 wgpu::ColorWriteMask::Blue)
                              : wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = shader;
    frag.entryPoint = "fs_canvas";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[8] = {};
    for (uint32_t i = 0; i < 8; i++) {
        attrs[i].format = wgpu::VertexFormat::Float32x4;
        attrs[i].offset = i * 16;
        attrs[i].shaderLocation = i;
    }
    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(Canvas2DInstance);
    vbl.stepMode = wgpu::VertexStepMode::Instance;
    vbl.attributeCount = 8;
    vbl.attributes = attrs;

    wgpu::DepthStencilState ds{};
    if (hasDepth) {
        ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::Always;
    }

    wgpu::RenderPipelineDescriptor desc{};
    desc.label = "Canvas2D";
    desc.layout = pipelineLayout;
    desc.vertex.module = shader;
    desc.vertex.entryPoint = "vs_canvas";
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vbl;
    desc.fragment = &frag;
    desc.depthStencil = hasDepth ? &ds : nullptr;
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    desc.multisample.count = samples;

    return pipelines.GetOrCreate(
        gpu.Device(),
        RenderPipelineCache::MakeKey(bs, format, ct.writeMask, hasDepth, samples, 0.0f, 0u), desc);
}

Canvas2D::Canvas2D() : mImpl(new Impl) {}

Canvas2D::~Canvas2D() { delete mImpl; }

int Canvas2D::Warm(uint32_t format, bool hasDepth, bool toTexture, uint32_t samples) {
    WgpuRnd* rnd = gWgpuRnd;
    if (!rnd || !rnd->Gpu().IsReady()) return 0;
    const uint64_t before = GpuPipelineCreateCount();
    const wgpu::TextureFormat fmt = (wgpu::TextureFormat)format;
    mImpl->PipelineFor(rnd->Gpu(), fmt, hasDepth, false, samples);
    // Whether the frame's alpha is the bloom mask is the frame's (a current
    // post-proc), so a pass that can carry it is warmed both ways.
    if (hasDepth && !toTexture) mImpl->PipelineFor(rnd->Gpu(), fmt, hasDepth, true, samples);
    return (int)(GpuPipelineCreateCount() - before);
}

bool Canvas2D::Begin(float logicalW, float logicalH) {
    if (mOpen) End();
    WgpuRnd* rnd = gWgpuRnd;
    if (!rnd || !rnd->IsInPass() || !rnd->Gpu().IsReady() || rnd->InShadowPass()) return false;
    if (!(logicalW > 0.0f) || !(logicalH > 0.0f)) return false;
    const int frame = rnd->FrameID();
    if (frame != mImpl->statsFrame) {
        mImpl->statsFrame = frame;
        mStats = FrameStats();
    }
    mImpl->builder.Begin(logicalW, logicalH, ReadTarget(*rnd));
    mOpen = true;
    return true;
}

void Canvas2D::End() {
    if (!mOpen) return;
    Flush();
    mImpl->builder.End();
    mOpen = false;
}

void Canvas2D::OnPassEnd() {
    if (mOpen) End();
}

void Canvas2D::OnViewportChanged() {
    if (!mOpen || mFlushing) return;
    WgpuRnd* rnd = gWgpuRnd;
    if (!rnd || !rnd->IsInPass()) return;
    mImpl->builder.SetTarget(ReadTarget(*rnd));
}

void Canvas2D::Quad(const Canvas2DQuad& q) {
    if (!mOpen) return;
    bool culled = false;
    if (mImpl->builder.Quad(q, &culled)) {
        mStats.quads++;
        mPending = true;
    } else if (culled) {
        mStats.culled++;
    }
}

void Canvas2D::FillRect(const Canvas2DRect& r, const Canvas2DColor& c, float radius) {
    Canvas2DQuad q;
    q.rect = r;
    q.radius = radius;
    q.SetColor(c);
    Quad(q);
}

void Canvas2D::PushClip(const Canvas2DRect& r) { mImpl->builder.PushClip(r); }

void Canvas2D::PopClip() { mImpl->builder.PopClip(); }

void Canvas2D::Flush() {
    if (!mPending || mFlushing) return;
    mFlushing = true;
    Impl& im = *mImpl;
    const std::vector<Canvas2DInstance>& insts = im.builder.Instances();
    const std::vector<Canvas2DBatch>& batches = im.builder.Batches();
    WgpuRnd* rnd = gWgpuRnd;
    // A pass that has gone (nothing ends one without OnPassEnd) drops them.
    if (rnd && rnd->IsInPass() && rnd->Gpu().IsReady() && !insts.empty()) {
        GpuDevice& gpu = rnd->Gpu();
        auto& dev = gpu.Device();
        im.EnsureResources(gpu);

        // This flush's range of the frame's arena. A full arena is replaced by
        // a larger one; draws already recorded keep the old buffer alive.
        const uint32_t n = (uint32_t)insts.size();
        const int frame = rnd->FrameID();
        if (frame != im.arenaFrame) {
            im.arenaFrame = frame;
            im.arenaUsed = 0;
        }
        if (!im.arena || im.arenaUsed + n > im.arenaCapacity) {
            uint32_t cap = im.arenaCapacity < 256 ? 256 : im.arenaCapacity * 2;
            while (cap < n) cap *= 2;
            wgpu::BufferDescriptor bd{};
            bd.label = "Canvas2DInstances";
            bd.size = (uint64_t)cap * sizeof(Canvas2DInstance);
            bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
            im.arena = dev.CreateBuffer(&bd);
            im.arenaCapacity = cap;
            im.arenaUsed = 0;
        }
        const uint64_t base = (uint64_t)im.arenaUsed * sizeof(Canvas2DInstance);
        im.arenaUsed += n;
        gpu.Queue().WriteBuffer(im.arena, base, insts.data(), (size_t)n * sizeof(Canvas2DInstance));

        const bool toTexture = rnd->ActiveTargetTex() != nullptr;
        const bool hasDepth = rnd->CurrentPassHasDepth();
        wgpu::RenderPipeline pipe =
            im.PipelineFor(gpu, rnd->CurrentTargetFormat(), hasDepth,
                           PassKeepsAlpha(hasDepth, toTexture), rnd->CurrentSampleCount());

        wgpu::RenderPassEncoder& pass = rnd->PassForCanvas2D();
        pass.SetPipeline(pipe);
        for (const Canvas2DBatch& b : batches) {
            if (b.count == 0 || b.scissor.Empty()) continue;
            // The game's texture as its material path resolves it: uploaded on
            // first use; one with no GPU backing draws untextured.
            wgpu::TextureView view;
            if (b.tex) {
                b.tex->PresyncBitmap();
                view = GetGpuTexView(b.tex);
            }
            if (!view) view = rnd->WhiteTexView();

            wgpu::BindGroupEntry bg[2] = {};
            bg[0].binding = 0;
            bg[0].textureView = view;
            bg[1].binding = 1;
            bg[1].sampler = im.sampler;
            wgpu::BindGroupDescriptor bgDesc{};
            bgDesc.layout = im.bindGroupLayout;
            bgDesc.entryCount = 2;
            bgDesc.entries = bg;
            // Same view + sampler is the same group: cached, and dropped with
            // the texture through WgpuRnd::ForgetTextureView.
            wgpu::BindGroup group = rnd->BindGroups().GetOrCreate(dev, bgDesc);

            pass.SetScissorRect((uint32_t)b.scissor.x, (uint32_t)b.scissor.y,
                                (uint32_t)b.scissor.w, (uint32_t)b.scissor.h);
            pass.SetBindGroup(0, group);
            pass.SetVertexBuffer(0, im.arena, base + (uint64_t)b.first * sizeof(Canvas2DInstance),
                                 (uint64_t)b.count * sizeof(Canvas2DInstance));
            pass.Draw(6, b.count);
            mStats.drawCalls++;
        }
        mStats.flushes++;

        // Leave the pass as the renderer keeps it: its own scissor, and the
        // scene bind group in slot 0 (as WgpuRnd::DrawRect restores it).
        int sc[4];
        rnd->PassScissor(sc);
        pass.SetScissorRect((uint32_t)sc[0], (uint32_t)sc[1], (uint32_t)sc[2], (uint32_t)sc[3]);
        if (rnd->SceneBindGroup()) pass.SetBindGroup(0, rnd->SceneBindGroup());
    }
    im.builder.ClearPending();
    mPending = false;
    mFlushing = false;
}

void Canvas2D::Terminate() {
    Impl& im = *mImpl;
    im.builder.End();
    im.builder.ClearPending();
    im.pipelines.Clear();
    im.shader = nullptr;
    im.bindGroupLayout = nullptr;
    im.pipelineLayout = nullptr;
    im.sampler = nullptr;
    im.arena = nullptr;
    im.arenaCapacity = 0;
    im.arenaUsed = 0;
    im.arenaFrame = -1;
    im.ready = false;
    mOpen = false;
    mPending = false;
    mFlushing = false;
}

Canvas2D* TheCanvas2D() { return gWgpuRnd ? &gWgpuRnd->Canvas() : nullptr; }

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* Canvas2DWgslSource() { return kCanvas2DShaderSource; }
