// RB3RetailPostDC3 — np-gfx-post: Dance Central 3's retail post chain on
// RB3RetailPost's bloom targets (RetailPostParams::dc3; see RB3RetailPost.h).
//
// Sources (all MEASURED, read from the shipped `xbox_shaders` microcode with
// xenia-gpu-shader-compiler, and from dc3-decomp's matched NgPostProc code):
//   bloom.ps         4 bilinear taps at uv +- 2 source texels (c15 = the
//                    source's texel size; c255 = (0.25, -2)), each rgb * a,
//                    averaged. Into w/4 that covers each 4x4 block exactly
//                    (RB3RetailPost's fs_bloom_down); into w/16 (the streak
//                    set1) it samples a quarter of each 16x16 block.
//   blur.ps (streak) 7 taps, SetBloomBlurWeightsStreak(horizontal, w, h,
//                    attenuation, pass, angle), dc3-decomp rndobj/Utl.cpp:
//                    weight[3] = 1/3, weight[3 +- i] = a^i / 3 with
//                    a = attenuation^(4^pass); offset 0.5 +- i * 4^pass texels;
//                    rotated by `angle`, the horizontal x scaled by the screen's
//                    height/width. Retail's 0.5 texel is D3D9's half-texel
//                    convention; centred here, as RB3RetailPost's 15-tap blur.
//   postprocess.ps   bit 4 bloom (tf7 + tf11 + tf15, c6), bit 15 chroma (c121),
//                    bit 43 chroma sharpen, bit 2 noise (tf13, c112, c113), bit
//                    47 midtone noise, bit 21 colour matrix (c92..c94), bit 36
//                    vignette (c123). Bit numbers from RndShaderPostProc::
//                    CalcShaderOpts (dc3-decomp rndobj/Shader.cpp).
#include "gfx/RB3RetailPost.h"
#include "gfx/GpuDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Up to seven taps: xy = uv offset, z = weight.
struct TapUniforms {
    float tap[7][4];
};
static_assert(sizeof(TapUniforms) == 112, "TapUniforms layout");

struct DC3CompositeUniforms {
    float xfm[3][4];
    float bloomColor[4];   // rgb = c6, w: 0 off, 1 bloom, 2 show mask, 3 show bloom term
    float vignette[4];     // c123: rgb colour, w intensity (0: off)
    float chroma[4];       // c121.xy = offset / (W, H); z = 1 on; w = 1 sharpen
    float noiseSeeds[4];   // c112
    float noiseParams[4];  // c113
    float noiseMode[4];    // x: 0 off, 1 overlay, 2 midtone
};
static_assert(sizeof(DC3CompositeUniforms) == 144, "DC3CompositeUniforms layout");

const char* kDC3Shader = R"WGSL(
struct TapUB {
    tap: array<vec4f, 7>,
};
struct DC3UB {
    xfm0: vec4f,
    xfm1: vec4f,
    xfm2: vec4f,
    bloomColor: vec4f,
    vignette: vec4f,
    chroma: vec4f,
    noiseSeeds: vec4f,
    noiseParams: vec4f,
    noiseMode: vec4f,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> tu: TapUB;

@group(0) @binding(6) var<uniform> cu: DC3UB;
@group(0) @binding(3) var bloom0: texture_2d<f32>;
@group(0) @binding(4) var bloom1: texture_2d<f32>;
@group(0) @binding(5) var bloom2: texture_2d<f32>;
@group(0) @binding(7) var noiseTex: texture_2d<f32>;
@group(0) @binding(8) var noiseSamp: sampler;

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

// bloom.ps with bilinear taps (tap[0..3].xy, weight 0.25 each): the scene
// weighted by its alpha, the pseudo-HDR mask.
@fragment fn fs_bloom_taps(in: VOut) -> @location(0) vec4f {
    var acc = vec3f(0.0);
    for (var i = 0; i < 4; i++) {
        let c = textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
        acc += c.rgb * c.a;
    }
    return vec4f(acc * 0.25, 1.0);
}

// blur.ps with SetBloomBlurWeightsStreak's seven taps.
@fragment fn fs_streak(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 7; i++) {
        acc += tu.tap[i].z * textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
    }
    return acc;
}

@fragment fn fs_composite_dc3(in: VOut) -> @location(0) vec4f {
    // Inspection views: the bloom mask (scene alpha) and the bloom term.
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

    // Bit 15 (chroma, c121): three taps on a triangle; bit 43 sharpens with them.
    var scene: vec3f;
    if (cu.chroma.z > 0.5) {
        let o = cu.chroma.xy;
        let t0 = textureSampleLevel(srcTex, samp, in.uv + vec2f(-o.x, 0.0), 0.0).rgb;
        let t1 = textureSampleLevel(srcTex, samp, in.uv + vec2f(0.5 * o.x, 0.866 * o.y), 0.0).rgb;
        let t2 = textureSampleLevel(srcTex, samp, in.uv + vec2f(0.5 * o.x, -0.866 * o.y), 0.0).rgb;
        if (cu.chroma.w > 0.5) {
            let center = textureSampleLevel(srcTex, samp, in.uv, 0.0).rgb;
            scene = 4.0 * center - t0 - t1 - t2;
        } else {
            scene = vec3f(t0.r, t1.g, t2.b);
        }
    } else {
        scene = textureSampleLevel(srcTex, samp, in.uv, 0.0).rgb;
    }

    // Bit 4: screen blend with the three bloom sets (unclamped).
    var c = scene;
    if (cu.bloomColor.w > 0.5) {
        let b = textureSampleLevel(bloom0, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom1, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom2, samp, in.uv, 0.0).rgb;
        c = 1.0 - (1.0 - b * cu.bloomColor.rgb) * (1.0 - c);
    }

    // Bits 2 / 47: the noise map, overlay-blended into the screen result S
    // (no clamp on S in the microcode). n = map(base uv) * map(top uv).
    if (cu.noiseMode.x > 0.5) {
        let baseUv = (in.uv + cu.noiseSeeds.xy) * cu.noiseParams.xy;
        let topUv = (in.uv + cu.noiseSeeds.zw) * cu.noiseParams.xy * cu.noiseParams.z;
        let n = textureSampleLevel(noiseTex, noiseSamp, baseUv, 0.0).rgb *
                textureSampleLevel(noiseTex, noiseSamp, topUv, 0.0).rgb;
        let s = c;
        let intensity = cu.noiseParams.w;
        if (cu.noiseMode.x < 1.5) {
            let lo = 2.0 * n * s;
            let hi = 1.0 - 2.0 * (1.0 - n) * (1.0 - s);
            c = mix(s, select(hi, lo, s < vec3f(0.5)), intensity);
        } else {
            let r = sqrt(n);
            let luma = dot(s, vec3f(0.3, 0.59, 0.11));
            let lo = 2.0 * r * s;
            let hi = 1.0 - 2.0 * (1.0 - r) * (1.0 - s);
            let k = intensity * 6.75 * luma * (1.0 - luma) * (1.0 - luma);
            c = s + k * (select(hi, lo, luma <= 0.5) - s);
        }
    }

    // Bit 21: dp4 against the colour matrix (saturating unless the vignette
    // follows, which saturates instead).
    let c4 = vec4f(c, 1.0);
    c = vec3f(dot(cu.xfm0, c4), dot(cu.xfm1, c4), dot(cu.xfm2, c4));

    // Bit 36: the vignette (c123), after the matrix.
    if (cu.vignette.w != 0.0) {
        let d = in.uv - 0.5;
        let w0 = clamp((dot(d, d) + cu.vignette.w) * 3.724 - 4.655, 0.0, 1.0);
        c = c + (w0 * w0) * (cu.vignette.rgb - c);
    }
    return vec4f(clamp(c, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

}  // namespace

void RB3RetailPost::EnsureDC3Pipelines(GpuDevice& gpu) {
    EnsurePipelines(gpu);
    if (mDC3Ready && mDC3FrameFormat == gpu.SurfaceFormat()) return;
    auto& dev = gpu.Device();
    mDC3FrameFormat = gpu.SurfaceFormat();

    wgpu::ShaderSourceWGSL src;
    src.code = kDC3Shader;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    smDesc.label = "DC3RetailPost";
    mDC3Shader = dev.CreateShaderModule(&smDesc);

    if (!mNoiseSampler) {
        // NgPostProc::CheckNoise: SetTextureFilter(0xd, 1) (linear),
        // SetTextureClamp(0xd, 0) (wrap); the map has no mips used.
        SamplerDesc nd{};
        nd.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        mNoiseSampler = gpu.GetSampler(nd);
    }

    auto texEntry = [](uint32_t b) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.texture.sampleType = wgpu::TextureSampleType::Float;
        e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        return e;
    };
    auto sampEntry = [](uint32_t b) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.sampler.type = wgpu::SamplerBindingType::Filtering;
        return e;
    };
    auto uniEntry = [](uint32_t b, uint64_t size) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.buffer.type = wgpu::BufferBindingType::Uniform;
        e.buffer.minBindingSize = size;
        return e;
    };
    {
        wgpu::BindGroupLayoutEntry e[3] = {texEntry(0), sampEntry(1), uniEntry(2, sizeof(TapUniforms))};
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 3;
        d.entries = e;
        mDC3PassBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::BindGroupLayoutEntry e[8] = {texEntry(0), sampEntry(1), uniEntry(6, sizeof(DC3CompositeUniforms)),
                                           texEntry(3), texEntry(4), texEntry(5), texEntry(7), sampEntry(8)};
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 8;
        d.entries = e;
        mDC3CompositeBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 1;
        d.bindGroupLayouts = &mDC3PassBGL;
        mDC3PassPL = dev.CreatePipelineLayout(&d);
        d.bindGroupLayouts = &mDC3CompositeBGL;
        mDC3CompositePL = dev.CreatePipelineLayout(&d);
    }
    auto make = [&](const char* fs, const wgpu::PipelineLayout& pl, wgpu::TextureFormat fmt) {
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mDC3Shader;
        frag.entryPoint = fs;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = fs;
        pd.layout = pl;
        pd.vertex.module = mDC3Shader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return dev.CreateRenderPipeline(&pd);
    };
    const wgpu::TextureFormat kBloomFmt = wgpu::TextureFormat::RGBA16Float;
    mDC3BloomTapsPipe = make("fs_bloom_taps", mDC3PassPL, kBloomFmt);
    mDC3StreakPipe = make("fs_streak", mDC3PassPL, kBloomFmt);
    mDC3CompositePipe = make("fs_composite_dc3", mDC3CompositePL, mDC3FrameFormat);
    mDC3Ready = true;
}

void RB3RetailPost::PassDC3(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
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
    bgd.layout = mDC3PassBGL;
    bgd.entryCount = 3;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3RetailBloom";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

// SetBloomBlurWeightsStreak (dc3-decomp rndobj/Utl.cpp), as uv offsets.
static TapUniforms StreakTaps(bool horizontal, float w, float h, float attenuation, int pass,
                              float angleDeg, float yRatio) {
    float weights[7], offsets[7];
    const float scale = std::pow(4.0f, (float)pass);
    const float atten = std::pow(attenuation, scale);
    weights[3] = 1.0f / 3.0f;
    offsets[3] = 0.0f;
    float curWeight = atten;
    for (int i = 1; i <= 3; i++) {
        weights[3 - i] = weights[3 + i] = curWeight * (1.0f / 3.0f);
        offsets[3 - i] = -(float)i * scale;
        offsets[3 + i] = (float)i * scale;
        curWeight *= atten;
    }
    const float a = angleDeg * 0.017453292f;
    const float s = std::sin(a), c = std::cos(a);
    TapUniforms t{};
    for (int i = 0; i < 7; i++) {
        if (horizontal) {
            const float off = offsets[i] / w;
            t.tap[i][0] = off * c * yRatio;
            t.tap[i][1] = off * s;
        } else {
            const float off = offsets[i] / h;
            t.tap[i][0] = -(off * s * yRatio);
            t.tap[i][1] = off * c;
        }
        t.tap[i][2] = weights[i];
    }
    return t;
}

void RB3RetailPost::RunDC3Bloom(wgpu::CommandEncoder& encoder, const wgpu::TextureView& bloomSrc,
                                int sceneW, int sceneH, const RetailPostParams& p, GpuDevice& gpu) {
    EnsureTextures(sceneW, sceneH, gpu);
    if (!p.streak) {
        // The plain arm is RB3's: bloom.ps into set0, then 15-tap blurs and
        // downsample_4x into set1 and set2 (identical in DC3's DoBloom).
        struct { float srcTexel[2], dir[2], lumaScale, pad[3]; } u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = 1.0f;
        Pass(encoder, mBloomDownPipe, mView[0][0], bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
        for (int s = 0; s < kSets; s++) {
            if (s > 0) {
                decltype(u) d{};
                d.srcTexel[0] = 1.0f / mW[s - 1];
                d.srcTexel[1] = 1.0f / mH[s - 1];
                Pass(encoder, mDown4Pipe, mView[s][0], mView[s - 1][0], &d.srcTexel[0], sizeof(d), gpu);
            }
            decltype(u) b{};
            b.srcTexel[0] = 1.0f / mW[s];
            b.srcTexel[1] = 1.0f / mH[s];
            b.dir[0] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][1], mView[s][0], &b.srcTexel[0], sizeof(b), gpu);
            b.dir[0] = 0.0f;
            b.dir[1] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][0], mView[s][1], &b.srcTexel[0], sizeof(b), gpu);
        }
        return;
    }
    // The streak arm. set0: bloom.ps into w/4 (exact 4x4 box), three streak
    // passes along the rotated horizontal, ping-ponging to end in [0][0].
    {
        struct { float srcTexel[2], dir[2], lumaScale, pad[3]; } u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = 1.0f;
        Pass(encoder, mBloomDownPipe, mView[0][1], bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
    }
    const wgpu::TextureView* ping[3][2] = {{&mView[0][1], &mView[0][0]},
                                           {&mView[0][0], &mView[0][1]},
                                           {&mView[0][1], &mView[0][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(true, (float)mW[0], (float)mH[0], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        PassDC3(encoder, mDC3StreakPipe, *ping[pass][1], *ping[pass][0], &t.tap[0][0], sizeof(t), gpu);
    }
    // set1: bloom.ps straight from the scene into w/16, then three passes
    // along the rotated vertical.
    {
        TapUniforms t{};
        const float tx = 2.0f / sceneW, ty = 2.0f / sceneH;
        const float o[4][2] = {{-tx, -ty}, {tx, ty}, {tx, -ty}, {-tx, ty}};
        for (int i = 0; i < 4; i++) {
            t.tap[i][0] = o[i][0];
            t.tap[i][1] = o[i][1];
            t.tap[i][2] = 0.25f;
        }
        PassDC3(encoder, mDC3BloomTapsPipe, mView[1][1], bloomSrc, &t.tap[0][0], sizeof(t), gpu);
    }
    const wgpu::TextureView* ping1[3][2] = {{&mView[1][1], &mView[1][0]},
                                            {&mView[1][0], &mView[1][1]},
                                            {&mView[1][1], &mView[1][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(false, (float)mW[1], (float)mH[1], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        PassDC3(encoder, mDC3StreakPipe, *ping1[pass][1], *ping1[pass][0], &t.tap[0][0], sizeof(t), gpu);
    }
}

void RB3RetailPost::RunDC3Composite(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                                    int sceneW, int sceneH, const wgpu::TextureView& frameView,
                                    const RetailPostParams& p, GpuDevice& gpu) {
    DC3CompositeUniforms cu{};
    std::memcpy(cu.xfm, p.xfm, sizeof(cu.xfm));
    cu.bloomColor[0] = p.bloomColor[0];
    cu.bloomColor[1] = p.bloomColor[1];
    cu.bloomColor[2] = p.bloomColor[2];
    cu.bloomColor[3] = p.debugView == 1 ? 2.0f : p.debugView == 2 ? 3.0f : p.bloom ? 1.0f : 0.0f;
    cu.vignette[0] = p.vignetteColor[0];
    cu.vignette[1] = p.vignetteColor[1];
    cu.vignette[2] = p.vignetteColor[2];
    cu.vignette[3] = p.vignetteIntensity;
    // CheckChromaticAberration: c121 = offset / (TheRnd.Width(), Height()).
    if (p.chromaticOffset != 0.0f) {
        cu.chroma[0] = p.chromaticOffset / (float)sceneW;
        cu.chroma[1] = p.chromaticOffset / (float)sceneH;
        cu.chroma[2] = 1.0f;
        cu.chroma[3] = p.chromaticSharpen;
    }
    const bool haveNoise = (bool)p.noiseMap;
    if (haveNoise) {
        std::memcpy(cu.noiseSeeds, p.noiseSeeds, sizeof(cu.noiseSeeds));
        std::memcpy(cu.noiseParams, p.noiseParams, sizeof(cu.noiseParams));
        cu.noiseMode[0] = p.noiseMidtone > 0.5f ? 2.0f : 1.0f;
    }
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, &cu, sizeof(cu));

    // The plain arm fills all three sets; the streak arm two (tf15 black).
    const bool haveBloom = (p.bloom || p.debugView == 2) && mView[0][0];
    const int setsUsed = p.streak ? 2 : 3;
    wgpu::BindGroupEntry e[8] = {};
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
        e[3 + s].textureView = (haveBloom && s < setsUsed) ? mView[s][0] : mBlackView;
    }
    e[6].binding = 7;
    e[6].textureView = haveNoise ? p.noiseMap : mBlackView;
    e[7].binding = 8;
    e[7].sampler = mNoiseSampler;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3CompositeBGL;
    bgd.entryCount = 8;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = frameView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3RetailPostComposite";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mDC3CompositePipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
#include "gfx/ShippedWgsl.h"
const char* DC3RetailPostWgslSource() { return kDC3Shader; }
