// RB3RetailPost — see RB3RetailPost.h for where each step comes from.
#include "gfx/RB3RetailPost.h"
#include "gfx/GpuDevice.h"

#include <algorithm>
#include <cstring>

namespace {

struct PassUniforms {
    float srcTexel[2];   // 1 / source size
    float dir[2];        // blur direction (1,0) or (0,1), in source texels
    float lumaScale;     // bloom mask scale (c7)
    float pad[3];
};
static_assert(sizeof(PassUniforms) == 32, "PassUniforms layout");

struct CompositeUniforms {
    float xfm[3][4];
    float bloomColor[4];      // rgb = c6, w = 1 when bloom is on (2/3: debugView 1/2)
    float vignetteColor[4];
    float misc0[4];           // vignetteIntensity, posterLevels, posterMin, chromaticOffset
    float misc1[4];           // chromaticSharpen, noiseIntensity, noiseMidtone, time
};
static_assert(sizeof(CompositeUniforms) == 112, "CompositeUniforms layout");

const char* kShader = R"WGSL(
struct PassUB {
    srcTexel: vec2f,
    dir: vec2f,
    lumaScale: f32,
    p0: f32, p1: f32, p2: f32,
};
struct CompUB {
    xfm0: vec4f,
    xfm1: vec4f,
    xfm2: vec4f,
    bloomColor: vec4f,
    vignetteColor: vec4f,
    misc0: vec4f,
    misc1: vec4f,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> pu: PassUB;

@group(0) @binding(6) var<uniform> cu: CompUB;
@group(0) @binding(3) var bloom0: texture_2d<f32>;
@group(0) @binding(4) var bloom1: texture_2d<f32>;
@group(0) @binding(5) var bloom2: texture_2d<f32>;

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

// bloom.ps: the scene weighted by its alpha, which standard.ps (mPseudoHDR)
// wrote as dot(rgb, c7.rgb) for AllowHDR materials and nothing else touched
// (cleared to 0). Here the 4x4 source block under each target texel is read
// exactly, where retail takes 4 bilinear taps over the same block.
@fragment fn fs_bloom_down(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4;
    var acc = vec3f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let c = textureLoad(srcTex, min(base + vec2i(i, j), dims - 1), 0);
            acc += c.rgb * c.a;
        }
    }
    return vec4f(acc / 16.0, 1.0);
}

// downsample_4x.ps: the 4x4 box under each target texel.
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

// blur.ps, 15 taps, weights from SetBloomBlurWeights. Retail's offsets are
// -6.5 .. 7.5 texels under D3D9's half-texel convention; centred here.
const kW = array<f32, 15>(0.0159283932, 0.0270778369, 0.0424231887, 0.0612547919,
                          0.0815124959, 0.0999667868, 0.1129886061, 0.1176957935,
                          0.1129886061, 0.0999667868, 0.0815124959, 0.0612547919,
                          0.0424231887, 0.0270778369, 0.0159283932);
@fragment fn fs_blur(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 15; i++) {
        let o = f32(i - 7) * pu.dir * pu.srcTexel;
        acc += kW[i] * textureSampleLevel(srcTex, samp, in.uv + o, 0.0);
    }
    return acc;
}

// The graded frame copied back over the main pass after the world-end flush
// (RB3RetailPost::Blit), one texel per pixel.
@fragment fn fs_copy(in: VOut) -> @location(0) vec4f {
    return vec4f(textureLoad(srcTex, vec2i(in.pos.xy), 0).rgb, 0.0);
}

@fragment fn fs_composite(in: VOut) -> @location(0) vec4f {
    let texSize = vec2f(textureDimensions(srcTex));
    var scene: vec3f;
    let chroma = cu.misc0.w;
    if (chroma > 0.0) {
        let off = chroma / texSize;
        let r = textureSampleLevel(srcTex, samp, in.uv + vec2f(off.x, 0.0), 0.0).r;
        let g = textureSampleLevel(srcTex, samp, in.uv, 0.0).g;
        let b = textureSampleLevel(srcTex, samp, in.uv - vec2f(off.x, 0.0), 0.0).b;
        if (cu.misc1.x > 0.5) {
            let center = textureSampleLevel(srcTex, samp, in.uv, 0.0).rgb;
            scene = center + (center - vec3f(r, g, b)) * 1.5;
        } else {
            scene = vec3f(r, g, b);
        }
    } else {
        scene = textureSampleLevel(srcTex, samp, in.uv, 0.0).rgb;
    }

    // postprocess.ps: screen-blend the three bloom sets, then the colour
    // transform (dp4_sat against c92..c94).
    // Inspection views (RetailPostParams::debugView): the bloom mask (scene
    // alpha), or the bloom term b * c6 the screen blend adds.
    if (cu.bloomColor.w > 2.5) {
        let b = textureSampleLevel(bloom0, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom1, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom2, samp, in.uv, 0.0).rgb;
        return vec4f(clamp(b * cu.bloomColor.rgb, vec3f(0.0), vec3f(1.0)), 1.0);
    }
    if (cu.bloomColor.w > 1.5) {
        let a = textureSampleLevel(srcTex, samp, in.uv, 0.0).a;
        return vec4f(a, a, a, 1.0);
    }
    var c = scene;
    if (cu.bloomColor.w > 0.5) {
        let b = textureSampleLevel(bloom0, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom1, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom2, samp, in.uv, 0.0).rgb;
        c = 1.0 - (1.0 - b * cu.bloomColor.rgb) * (1.0 - c);
    }
    let c4 = vec4f(c, 1.0);
    c = clamp(vec3f(dot(cu.xfm0, c4), dot(cu.xfm1, c4), dot(cu.xfm2, c4)),
              vec3f(0.0), vec3f(1.0));

    let posterLevels = cu.misc0.y;
    if (posterLevels > 1.0) {
        let intensity = max(max(c.r, c.g), c.b);
        if (intensity >= cu.misc0.z) {
            c = floor(c * posterLevels + 0.5) / posterLevels;
        }
    }
    let vig = cu.misc0.x;
    if (vig > 0.0) {
        let dist = length(in.uv - 0.5) * 1.414;
        let k = 1.0 - smoothstep(0.4, 1.0, dist) * vig;
        c = mix(cu.vignetteColor.rgb, c, k);
    }
    let noise = cu.misc1.y;
    if (noise != 0.0) {
        let px = in.uv * texSize;
        let n1 = fract(sin(dot(px + cu.misc1.w * 43.17, vec2f(12.9898, 78.233))) * 43758.5453);
        let nv = (n1 - 0.5) * noise;
        if (cu.misc1.z > 0.5) {
            let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
            c = c + nv * 4.0 * l * (1.0 - l);
        } else {
            c = c + nv;
        }
    }
    return vec4f(clamp(c, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

}  // namespace

void RB3RetailPost::Init(GpuDevice& gpu) {
    SamplerDesc sd{};
    sd.addressU = wgpu::AddressMode::ClampToEdge;
    sd.addressV = wgpu::AddressMode::ClampToEdge;
    mSampler = gpu.GetSampler(sd);
}

void RB3RetailPost::EnsurePipelines(GpuDevice& gpu) {
    if (mReady && mFrameFormat == gpu.SurfaceFormat()) return;
    auto& dev = gpu.Device();
    if (!mSampler) Init(gpu);
    mFrameFormat = gpu.SurfaceFormat();

    wgpu::ShaderSourceWGSL src;
    src.code = kShader;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    smDesc.label = "RB3RetailPost";
    mShader = dev.CreateShaderModule(&smDesc);

    auto texEntry = [](uint32_t b) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.texture.sampleType = wgpu::TextureSampleType::Float;
        e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        return e;
    };
    wgpu::BindGroupLayoutEntry samp{};
    samp.binding = 1;
    samp.visibility = wgpu::ShaderStage::Fragment;
    samp.sampler.type = wgpu::SamplerBindingType::Filtering;

    {
        wgpu::BindGroupLayoutEntry e[3] = {texEntry(0), samp, {}};
        e[2].binding = 2;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].buffer.type = wgpu::BufferBindingType::Uniform;
        e[2].buffer.minBindingSize = sizeof(PassUniforms);
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 3;
        d.entries = e;
        mPassBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::BindGroupLayoutEntry e[6] = {texEntry(0), samp, {}, texEntry(3), texEntry(4), texEntry(5)};
        e[2].binding = 6;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].buffer.type = wgpu::BufferBindingType::Uniform;
        e[2].buffer.minBindingSize = sizeof(CompositeUniforms);
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 6;
        d.entries = e;
        mCompositeBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 1;
        d.bindGroupLayouts = &mPassBGL;
        mPassPL = dev.CreatePipelineLayout(&d);
        d.bindGroupLayouts = &mCompositeBGL;
        mCompositePL = dev.CreatePipelineLayout(&d);
    }

    auto make = [&](const char* fs, const wgpu::PipelineLayout& pl, wgpu::TextureFormat fmt) {
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = fs;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return dev.CreateRenderPipeline(&pd);
    };
    const wgpu::TextureFormat kBloomFmt = wgpu::TextureFormat::RGBA16Float;
    mBloomDownPipe = make("fs_bloom_down", mPassPL, kBloomFmt);
    mDown4Pipe = make("fs_down4", mPassPL, kBloomFmt);
    mBlurPipe = make("fs_blur", mPassPL, kBloomFmt);
    mCompositePipe = make("fs_composite", mCompositePL, mFrameFormat);

    wgpu::BufferDescriptor bd{};
    bd.size = (uint64_t)kSlots * kSlotStride;
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = dev.CreateBuffer(&bd);
    mSlot = 0;

    {
        wgpu::TextureDescriptor td{};
        td.label = "RB3RetailPostBlack";
        td.size = {1, 1, 1};
        td.format = kBloomFmt;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        mBlackTex = dev.CreateTexture(&td);
        const uint16_t zero[4] = {0, 0, 0, 0};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mBlackTex;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 8;
        layout.rowsPerImage = 1;
        wgpu::Extent3D ext = {1, 1, 1};
        gpu.Queue().WriteTexture(&dst, zero, sizeof(zero), &layout, &ext);
        mBlackView = mBlackTex.CreateView();
    }
    mReady = true;
}

void RB3RetailPost::EnsureTextures(int sceneW, int sceneH, GpuDevice& gpu) {
    int w = sceneW, h = sceneH;
    for (int s = 0; s < kSets; s++) {
        // BloomTextures<3>::AllocateTextures: each set a quarter of the last.
        w = std::max(1, w >> 2);
        h = std::max(1, h >> 2);
        if (mW[s] == w && mH[s] == h && mTex[s][0]) continue;
        for (int k = 0; k < 2; k++) {
            wgpu::TextureDescriptor td{};
            td.label = "RB3RetailBloom";
            td.size = {(uint32_t)w, (uint32_t)h, 1};
            td.format = wgpu::TextureFormat::RGBA16Float;
            td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            mTex[s][k] = gpu.Device().CreateTexture(&td);
            mView[s][k] = mTex[s][k].CreateView();
        }
        mW[s] = w;
        mH[s] = h;
    }
}

void RB3RetailPost::Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                         const wgpu::TextureView& dst, const wgpu::TextureView& src,
                         const float* uni, size_t uniBytes, GpuDevice& gpu) {
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, uni, uniBytes);

    wgpu::BindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].textureView = src;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 2;
    e[2].buffer = mUniforms;
    e[2].offset = off;
    e[2].size = uniBytes;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPassBGL;
    bgd.entryCount = 3;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "RB3RetailBloom";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

void RB3RetailPost::Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                        int sceneW, int sceneH, const wgpu::TextureView& frameView,
                        const RetailPostParams& p, GpuDevice& gpu) {
    if (!sceneView || !frameView || sceneW <= 0 || sceneH <= 0) return;
    EnsurePipelines(gpu);

    if (p.bloom) {
        EnsureTextures(sceneW, sceneH, gpu);
        // NgPostProc::DoBloom, the plain (no glare, no streak) arm.
        PassUniforms u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = p.bloomLumaScale;
        Pass(encoder, mBloomDownPipe, mView[0][0], sceneView, &u.srcTexel[0], sizeof(u), gpu);
        for (int s = 0; s < kSets; s++) {
            if (s > 0) {
                PassUniforms d{};
                d.srcTexel[0] = 1.0f / mW[s - 1];
                d.srcTexel[1] = 1.0f / mH[s - 1];
                Pass(encoder, mDown4Pipe, mView[s][0], mView[s - 1][0], &d.srcTexel[0], sizeof(d), gpu);
            }
            PassUniforms b{};
            b.srcTexel[0] = 1.0f / mW[s];
            b.srcTexel[1] = 1.0f / mH[s];
            b.dir[0] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][1], mView[s][0], &b.srcTexel[0], sizeof(b), gpu);
            b.dir[0] = 0.0f;
            b.dir[1] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][0], mView[s][1], &b.srcTexel[0], sizeof(b), gpu);
        }
    }

    CompositeUniforms cu{};
    std::memcpy(cu.xfm, p.xfm, sizeof(cu.xfm));
    cu.bloomColor[0] = p.bloomColor[0];
    cu.bloomColor[1] = p.bloomColor[1];
    cu.bloomColor[2] = p.bloomColor[2];
    cu.bloomColor[3] = p.debugView ? 1.0f + (float)p.debugView : p.bloom ? 1.0f : 0.0f;
    std::memcpy(cu.vignetteColor, p.vignetteColor, sizeof(cu.vignetteColor));
    cu.misc0[0] = p.vignetteIntensity;
    cu.misc0[1] = p.posterLevels;
    cu.misc0[2] = p.posterMin;
    cu.misc0[3] = p.chromaticOffset;
    cu.misc1[0] = p.chromaticSharpen;
    cu.misc1[1] = p.noiseIntensity;
    cu.misc1[2] = p.noiseMidtone;
    cu.misc1[3] = p.time;
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, &cu, sizeof(cu));

    const bool haveBloom = p.bloom && mView[0][0];
    wgpu::BindGroupEntry e[6] = {};
    e[0].binding = 0;
    e[0].textureView = sceneView;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 6;
    e[2].buffer = mUniforms;
    e[2].offset = off;
    e[2].size = sizeof(cu);
    for (int s = 0; s < kSets; s++) {
        e[3 + s].binding = 3 + s;
        e[3 + s].textureView = haveBloom ? mView[s][0] : mBlackView;
    }
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mCompositeBGL;
    bgd.entryCount = 6;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = frameView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "RB3RetailPostComposite";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mCompositePipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

void RB3RetailPost::Blit(wgpu::RenderPassEncoder& pass, const wgpu::TextureView& src,
                         uint32_t samples, wgpu::TextureFormat depthFmt, GpuDevice& gpu) {
    EnsurePipelines(gpu);
    auto& dev = gpu.Device();
    if (!mBlitPipe || mBlitSamples != samples || mBlitDepth != depthFmt) {
        wgpu::ColorTargetState ct{};
        ct.format = mFrameFormat;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = "fs_copy";
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::DepthStencilState ds{};
        ds.format = depthFmt;
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::Always;
        ds.stencilFront.compare = wgpu::CompareFunction::Always;
        ds.stencilBack.compare = wgpu::CompareFunction::Always;
        ds.stencilReadMask = 0;
        ds.stencilWriteMask = 0;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "RB3RetailBlit";
        pd.layout = mPassPL;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pd.depthStencil = depthFmt == wgpu::TextureFormat::Undefined ? nullptr : &ds;
        pd.multisample.count = samples;
        mBlitPipe = dev.CreateRenderPipeline(&pd);
        mBlitSamples = samples;
        mBlitDepth = depthFmt;
    }
    PassUniforms pu{};
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, &pu, sizeof(pu));
    wgpu::BindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].textureView = src;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 2;
    e[2].buffer = mUniforms;
    e[2].offset = off;
    e[2].size = sizeof(pu);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPassBGL;
    bgd.entryCount = 3;
    bgd.entries = e;
    wgpu::BindGroup bg = dev.CreateBindGroup(&bgd);
    pass.SetPipeline(mBlitPipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
}

void RB3RetailPost::Terminate() {
    mBlitPipe = nullptr;
    mBlitSamples = 0;
    for (int s = 0; s < kSets; s++) {
        for (int k = 0; k < 2; k++) {
            mTex[s][k] = nullptr;
            mView[s][k] = nullptr;
        }
        mW[s] = mH[s] = 0;
    }
    mShader = nullptr;
    mPassBGL = nullptr;
    mCompositeBGL = nullptr;
    mPassPL = nullptr;
    mCompositePL = nullptr;
    mBloomDownPipe = nullptr;
    mDown4Pipe = nullptr;
    mBlurPipe = nullptr;
    mCompositePipe = nullptr;
    mUniforms = nullptr;
    mBlackTex = nullptr;
    mBlackView = nullptr;
    mSampler = nullptr;
    mReady = false;
}
