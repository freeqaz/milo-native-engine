#include "gfx/DofPass.h"
#include "gfx/GpuDevice.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Cam.h"
#include "rndobj/PostProc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct BlurUniforms {
    float taps[8][4];  // xy = uv offset (SetVHBlurWeights: c31..c38)
};
static_assert(sizeof(BlurUniforms) == 128, "BlurUniforms layout");

struct DofUniforms {
    float c24[4];  // range, -scale * range, minBlur, maxBlur
};
static_assert(sizeof(DofUniforms) == 16, "DofUniforms layout");

// The two tap tables SetVHBlurWeights builds on first use (function-local
// statics in retail). Both are 2D; "horizontal" and "vertical" name the passes,
// not the directions.
const float kHorzTaps[8][2] = {
    {0.4432332516f, -0.9751155376f}, {0.5374298096f, -0.4737342f},
    {-0.2649691105f, -0.4189302325f}, {0.7919751406f, 0.1909018755f},
    {-0.2418884039f, 0.9970650673f}, {-0.8140995502f, 0.9143759012f},
    {0.1998412609f, 0.7864136696f}, {0.1438316107f, -0.1410079002f},
};
const float kVertTaps[8][2] = {
    {-0.9420162439f, -0.3990621567f}, {0.9455860853f, -0.768907249f},
    {-0.09418410063f, -0.9293887019f}, {0.3449593782f, 0.2938776016f},
    {-0.9158858061f, 0.4577143192f}, {-0.8154423237f, -0.8791246414f},
    {-0.3827754259f, 0.276768446f}, {0.9748439789f, 0.7564837933f},
};

const char* kDofShaderSource = R"WGSL(
struct BlurUB {
    taps: array<vec4f, 8>,
};
struct DofUB {
    c24: vec4f,   // range, -scale * range, minBlur, maxBlur
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> blur: BlurUB;

@group(0) @binding(3) var sceneTex: texture_2d<f32>;
@group(0) @binding(4) var blurTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_multisampled_2d;
@group(0) @binding(6) var<uniform> dof: DofUB;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

// downsample_4x.ps: the 4x4 box under each target texel (retail takes four
// bilinear taps over the same block).
@fragment fn fs_down4(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            acc += textureLoad(srcTex, min(base + vec2i(i, j), dims - 1), 0);
        }
    }
    return acc / 16.0;
}

// blur.ps, 8-tap permutation: sum of c47..c54 (all 0.125) times the source at
// uv + c31..c38. No centre tap.
@fragment fn fs_blur(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 8; i++) {
        acc += textureSampleLevel(srcTex, samp, in.uv + blur.taps[i].xy, 0.0);
    }
    return acc * 0.125;
}

// postprocess.ps, depth-of-field bit: tf6 scene, tf8 blur (linear, clamp),
// tf9 depth. Retail reads 1 - z because it runs reverse-Z.
@fragment fn fs_composite(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let blurred = textureSampleLevel(blurTex, samp, in.uv, 0.0);
    let z = textureLoad(depthTex, min(p, vec2i(textureDimensions(depthTex)) - 1), 0);
    let t = z * dof.c24.x + dof.c24.y;
    let f = clamp(min(max(abs(t), dof.c24.z), dof.c24.w), 0.0, 1.0);
    return clamp(mix(scene, blurred, f), vec4f(0.0), vec4f(1.0));
}
)WGSL";

// Projected depth of view distance d, as DxCam::ProjectZ before its reverse-Z
// flip; 0 when d is nearer than the near plane (NgDOFProc::Set's guard).
float ProjectedDepth(float d, float nearPlane, float farPlane, float zNear, float zFar) {
    if (d < nearPlane) return 0.0f;
    return (farPlane - farPlane / d * nearPlane) / (farPlane - nearPlane) * (zFar - zNear) + zNear;
}

}  // namespace

void DofPass::RetailConstants(float focalPlane, float blurDepth, float maxBlur, float minBlur,
                              float nearPlane, float farPlane, float zNear, float zFar,
                              float out[4]) {
    float scale = ProjectedDepth(focalPlane, nearPlane, farPlane, zNear, zFar);
    float bias = ProjectedDepth(focalPlane - focalPlane * blurDepth, nearPlane, farPlane,
                                zNear, zFar);
    if (scale < bias + 0.001f) scale = bias + 0.001f;
    float range = 1.0f / (scale - bias);
    out[0] = range;
    out[1] = -(scale * range);
    out[2] = std::min(maxBlur, minBlur);
    out[3] = maxBlur < 0.0f ? 1.0f : maxBlur;
}

void DofPass::RetailBlurOffsets(bool vertical, int blurW, int blurH, float blurWidthScale,
                                float out[8][2]) {
    const float (*taps)[2] = vertical ? kVertTaps : kHorzTaps;
    const float f = blurWidthScale * 0.666f;
    const float xScale = (float)blurW * f * 4.8828124e-06f;
    const float yScale = (float)blurH * f * 1.5432099e-05f;
    for (int i = 0; i < 8; i++) {
        out[i][0] = taps[i][0] * xScale * 5.0f;
        out[i][1] = taps[i][1] * yScale * 5.0f;
    }
}

void DofPass::EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu) {
    if (mShader && mFormat == format) return;
    auto& dev = gpu.Device();
    mFormat = format;

    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kDofShaderSource;
        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.nextInChain = &src;
        smDesc.label = "DofPass";
        mShader = dev.CreateShaderModule(&smDesc);

        SamplerDesc sd{};
        sd.addressU = wgpu::AddressMode::ClampToEdge;
        sd.addressV = wgpu::AddressMode::ClampToEdge;
        mSampler = gpu.GetSampler(sd);

        auto tex = [](uint32_t b) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = b;
            e.visibility = wgpu::ShaderStage::Fragment;
            e.texture.sampleType = wgpu::TextureSampleType::Float;
            e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            return e;
        };
        auto ub = [](uint32_t b, uint64_t size) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = b;
            e.visibility = wgpu::ShaderStage::Fragment;
            e.buffer.type = wgpu::BufferBindingType::Uniform;
            e.buffer.minBindingSize = size;
            return e;
        };
        wgpu::BindGroupLayoutEntry samp{};
        samp.binding = 1;
        samp.visibility = wgpu::ShaderStage::Fragment;
        samp.sampler.type = wgpu::SamplerBindingType::Filtering;
        // The depth is read with textureLoad only, so it needs no sampler and
        // no filterable sample type.
        wgpu::BindGroupLayoutEntry depth{};
        depth.binding = 5;
        depth.visibility = wgpu::ShaderStage::Fragment;
        depth.texture.sampleType = wgpu::TextureSampleType::Depth;
        depth.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        depth.texture.multisampled = true;

        {
            wgpu::BindGroupLayoutEntry e[3] = {tex(0), samp, ub(2, sizeof(BlurUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofPassBGL";
            d.entryCount = 3;
            d.entries = e;
            mPassBGL = dev.CreateBindGroupLayout(&d);
        }
        {
            wgpu::BindGroupLayoutEntry e[5] = {samp, tex(3), tex(4), depth,
                                               ub(6, sizeof(DofUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofCompositeBGL";
            d.entryCount = 5;
            d.entries = e;
            mCompositeBGL = dev.CreateBindGroupLayout(&d);
        }
        wgpu::PipelineLayoutDescriptor pl{};
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &mPassBGL;
        mPassPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mCompositeBGL;
        mCompositePL = dev.CreatePipelineLayout(&pl);

        wgpu::BufferDescriptor bd{};
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        bd.size = sizeof(BlurUniforms);
        bd.label = "DofBlurH";
        mBlurUB[0] = dev.CreateBuffer(&bd);
        bd.label = "DofBlurV";
        mBlurUB[1] = dev.CreateBuffer(&bd);
        bd.size = sizeof(DofUniforms);
        bd.label = "DofComposite";
        mCompositeUB = dev.CreateBuffer(&bd);
    }

    auto make = [&](const wgpu::PipelineLayout& layout, const char* fs, const char* label) {
        wgpu::ColorTargetState ct{};
        ct.format = format;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = fs;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = label;
        pd.layout = layout;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return dev.CreateRenderPipeline(&pd);
    };
    mDownPipe = make(mPassPL, "fs_down4", "DofDownsample4x");
    mBlurPipe = make(mPassPL, "fs_blur", "DofBlur");
    mCompositePipe = make(mCompositePL, "fs_composite", "DofComposite");
    // The quarter and output textures are in `format` too.
    mW = mH = 0;
}

void DofPass::EnsureTextures(int w, int h, wgpu::TextureFormat format, GpuDevice& gpu) {
    if (mW == w && mH == h && mOutTex) return;
    auto& dev = gpu.Device();
    // NgDOFProc's constructor: blur targets are the pre-process texture >> 2.
    const uint32_t qw = (uint32_t)std::max(1, w >> 2);
    const uint32_t qh = (uint32_t)std::max(1, h >> 2);
    for (int k = 0; k < 2; k++) {
        wgpu::TextureDescriptor td{};
        td.label = k ? "DofBlurB" : "DofBlurA";
        td.size = {qw, qh, 1};
        td.format = format;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mQuarter[k] = dev.CreateTexture(&td);
        mQuarterView[k] = mQuarter[k].CreateView();
    }
    wgpu::TextureDescriptor od{};
    od.label = "DofOut";
    od.size = {(uint32_t)w, (uint32_t)h, 1};
    od.format = format;
    od.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc;  // CopySrc: readback in tests
    mOutTex = dev.CreateTexture(&od);
    mOutView = mOutTex.CreateView();
    mW = w;
    mH = h;
}

void DofPass::Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                   const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label) {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = label;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

bool DofPass::Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                  wgpu::TextureFormat sceneFormat, const wgpu::TextureView& depthView,
                  uint32_t depthSamples, int sceneW, int sceneH, GpuDevice& gpu) {
    extern DOFProc* TheDOFProc;
    if (!TheDOFProc || !TheDOFProc->Enabled()) return false;
    if (!sceneView || !depthView || sceneW <= 0 || sceneH <= 0) return false;
    if (depthSamples < 2) {
        // The composite binds the depth as texture_depth_multisampled_2d.
        if (!mWarnedSamples) {
            fprintf(stderr, "DofPass: single-sample depth is not supported; depth of field off\n");
            mWarnedSamples = true;
        }
        return false;
    }
    EnsurePipelines(sceneFormat, gpu);
    EnsureTextures(sceneW, sceneH, sceneFormat, gpu);
    auto& dev = gpu.Device();
    auto& queue = gpu.Queue();

    const int qw = std::max(1, sceneW >> 2);
    const int qh = std::max(1, sceneH >> 2);
    const float widthScale = RndPostProc::DOFOverrides().mBlurWidthScale;
    for (int k = 0; k < 2; k++) {
        float off[8][2];
        RetailBlurOffsets(k == 1, qw, qh, widthScale, off);
        BlurUniforms bu{};
        for (int i = 0; i < 8; i++) {
            bu.taps[i][0] = off[i][0];
            bu.taps[i][1] = off[i][1];
            bu.taps[i][2] = 1.0f;
            bu.taps[i][3] = 1.0f;
        }
        queue.WriteBuffer(mBlurUB[k], 0, &bu, sizeof(bu));
    }

    RndCam* cam = RndCam::Current();
    const float nearPlane = cam ? cam->NearPlane() : 1.0f;
    const float farPlane = cam ? cam->FarPlane() : 1000.0f;
    const Vector2 zr = cam ? rndshape::CamZRange(cam) : Vector2(0.0f, 1.0f);
    DofUniforms du{};
    RetailConstants(TheDOFProc->FocalPlane(), TheDOFProc->BlurDepth(), TheDOFProc->MaxBlur(),
                    TheDOFProc->MinBlur(), nearPlane, farPlane, zr.x, zr.y, du.c24);
    queue.WriteBuffer(mCompositeUB, 0, &du, sizeof(du));

    auto passBG = [&](const wgpu::TextureView& src, const wgpu::Buffer& ub) {
        wgpu::BindGroupEntry e[3] = {};
        e[0].binding = 0;
        e[0].textureView = src;
        e[1].binding = 1;
        e[1].sampler = mSampler;
        e[2].binding = 2;
        e[2].buffer = ub;
        e[2].size = sizeof(BlurUniforms);
        wgpu::BindGroupDescriptor d{};
        d.layout = mPassBGL;
        d.entryCount = 3;
        d.entries = e;
        return dev.CreateBindGroup(&d);
    };

    // 1. downsample_4x: scene -> A.  2. blur, horizontal taps: A -> B.
    // 3. blur, vertical taps: B -> A.
    Pass(encoder, mDownPipe, passBG(sceneView, mBlurUB[0]), mQuarterView[0], "DofDownsample4x");
    Pass(encoder, mBlurPipe, passBG(mQuarterView[0], mBlurUB[0]), mQuarterView[1], "DofBlurH");
    Pass(encoder, mBlurPipe, passBG(mQuarterView[1], mBlurUB[1]), mQuarterView[0], "DofBlurV");

    // 4. The composite: scene, A, depth -> output.
    {
        wgpu::BindGroupEntry e[5] = {};
        e[0].binding = 1;
        e[0].sampler = mSampler;
        e[1].binding = 3;
        e[1].textureView = sceneView;
        e[2].binding = 4;
        e[2].textureView = mQuarterView[0];
        e[3].binding = 5;
        e[3].textureView = depthView;
        e[4].binding = 6;
        e[4].buffer = mCompositeUB;
        e[4].size = sizeof(DofUniforms);
        wgpu::BindGroupDescriptor d{};
        d.layout = mCompositeBGL;
        d.entryCount = 5;
        d.entries = e;
        Pass(encoder, mCompositePipe, dev.CreateBindGroup(&d), mOutView, "DofComposite");
    }
    return true;
}

void DofPass::Terminate() {
    mShader = nullptr;
    mPassBGL = nullptr;
    mCompositeBGL = nullptr;
    mPassPL = nullptr;
    mCompositePL = nullptr;
    mDownPipe = nullptr;
    mBlurPipe = nullptr;
    mCompositePipe = nullptr;
    mSampler = nullptr;
    mBlurUB[0] = mBlurUB[1] = nullptr;
    mCompositeUB = nullptr;
    mQuarter[0] = mQuarter[1] = nullptr;
    mQuarterView[0] = mQuarterView[1] = nullptr;
    mOutTex = nullptr;
    mOutView = nullptr;
    mW = mH = 0;
    mFormat = wgpu::TextureFormat::Undefined;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* DofPassWgslSource() { return kDofShaderSource; }
