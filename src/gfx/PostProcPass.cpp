#include "gfx/PostProcPass.h"
#include "gfx/NoiseRows.h"
#include "gfx/BindGroupCount.h"
#include "gfx/GpuTimers.h"
#include "gfx/GpuDevice.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"
#include "rndobj/Tex.h"
#include "platform/rndshape/RndShape.h"
#include "gfx/RB3RetailPost.h"
#include "platform/TexGpu.h"
#include "platform/LookProfile.h"
#include <cstdio>
#include <cmath>
#include <utility>

#include <cstring>
#include <cstdlib>
#include "gfx/PipelineCount.h"

struct PostProcUniforms {
    float contrast;
    float brightness;
    float saturation;
    float vignetteIntensity;
    float vignetteColor[4];
    float chromaticOffset;
    float chromaticSharpen;
    float posterLevels;
    float posterMin;
    float levelInLo[4];
    float levelInHi[4];
    float levelOutLo[4];
    float levelOutHi[4];
    float bloomIntensity;
    float noiseIntensity;
    float noiseMidtone;
    float flickerMul;
    float bloomColor[4];
    float time;
    float noiseMode;     // 0 procedural grain (noiseIntensity), 1 map overlay, 2 map midtone
    // np-look23 (a), NoiseRows.h: 0 = the authored uv; otherwise the map was
    // retiled to output rows and each layer samples d past its texel's
    // centre, noiseSnapBase = 1 + base d, noiseSnapTop = top d.
    float noiseSnapBase;
    float noiseSnapTop;
    float noiseSeeds[4];   // retail kPS_NoiseSeeds (c112)
    float noiseParams[4];  // retail kPS_NoiseParams (c113): baseScale.xy, topScale, intensity
};
static_assert(sizeof(PostProcUniforms) == 192, "PostProcUniforms must be 192 bytes");

static const char* kPostProcShaderSource = R"WGSL(
struct PostProcUB {
    contrast: f32,
    brightness: f32,
    saturation: f32,
    vignetteIntensity: f32,
    vignetteColor: vec4f,
    chromaticOffset: f32,
    chromaticSharpen: f32,
    posterLevels: f32,
    posterMin: f32,
    levelInLo: vec4f,
    levelInHi: vec4f,
    levelOutLo: vec4f,
    levelOutHi: vec4f,
    bloomIntensity: f32,
    noiseIntensity: f32,
    noiseMidtone: f32,
    flickerMul: f32,
    bloomColor: vec4f,
    time: f32,
    noiseMode: f32,
    noiseSnapBase: f32,
    noiseSnapTop: f32,
    noiseSeeds: vec4f,
    noiseParams: vec4f,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var sceneSampler: sampler;
@group(0) @binding(2) var<uniform> pp: PostProcUB;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
@group(0) @binding(4) var noiseTex: texture_2d<f32>;
@group(0) @binding(5) var noiseSampler: sampler;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_postproc(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_postproc(in: VOut) -> @location(0) vec4f {
    let texSize = vec2f(textureDimensions(sceneTex));

    var color: vec3f;
    if (pp.chromaticOffset > 0.0) {
        let offset = pp.chromaticOffset / texSize;
        let r = textureSample(sceneTex, sceneSampler, in.uv + vec2f(offset.x, 0.0)).r;
        let g = textureSample(sceneTex, sceneSampler, in.uv).g;
        let b = textureSample(sceneTex, sceneSampler, in.uv - vec2f(offset.x, 0.0)).b;
        if (pp.chromaticSharpen > 0.5) {
            let center = textureSample(sceneTex, sceneSampler, in.uv).rgb;
            let blur = vec3f(r, g, b);
            color = center + (center - blur) * 1.5;
        } else {
            color = vec3f(r, g, b);
        }
    } else {
        color = textureSample(sceneTex, sceneSampler, in.uv).rgb;
    }

    let inRange = max(pp.levelInHi.rgb - pp.levelInLo.rgb, vec3f(0.001));
    let normalized = clamp((color - pp.levelInLo.rgb) / inRange, vec3f(0.0), vec3f(1.0));
    color = mix(pp.levelOutLo.rgb, pp.levelOutHi.rgb, normalized);

    // Match Xbox's non-linear contrast formula (from RndColorXfm::AdjustContrast)
    var contrastMul: f32;
    let contrastNorm = pp.contrast / 100.0;
    if (contrastNorm > 0.0) {
        contrastMul = 1.0 / (contrastNorm * -0.9921875 + 1.0);
    } else {
        contrastMul = -(contrastNorm * -0.992126 - 1.0);
    }
    let contrastOff = (1.0 - contrastMul) * 0.5;
    color = color * contrastMul + contrastOff;
    // Brightness: match Xbox formula
    let brightnessAdj = (pp.brightness + 100.0) / 200.0 - 0.5;
    color = color + brightnessAdj;

    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    color = mix(vec3f(luma), color, 1.0 + pp.saturation / 100.0);

    if (pp.posterLevels > 1.0) {
        let levels = pp.posterLevels;
        let intensity = max(max(color.r, color.g), color.b);
        if (intensity >= pp.posterMin) {
            color = floor(color * levels + 0.5) / levels;
        }
    }

    if (pp.vignetteIntensity > 0.0) {
        let center = in.uv - 0.5;
        let dist = length(center) * 1.414;
        let vig = 1.0 - smoothstep(0.4, 1.0, dist) * pp.vignetteIntensity;
        color = mix(pp.vignetteColor.rgb, color, vig);
    }

    // Flicker: time-based brightness modulation
    if (pp.flickerMul != 1.0) {
        color = color * pp.flickerMul;
    }

    // Noise/grain: procedural hash-based noise, for a consumer whose post-process
    // exposes no noise map (noiseMode 0). DC3 takes the map path below instead.
    if (pp.noiseMode < 0.5 && pp.noiseIntensity != 0.0) {
        let px = in.uv * vec2f(textureDimensions(sceneTex));
        let n1 = fract(sin(dot(px + pp.time * 43.17, vec2f(12.9898, 78.233))) * 43758.5453);
        let noise = (n1 - 0.5) * pp.noiseIntensity;
        if (pp.noiseMidtone > 0.5) {
            // Overlay blend: noise affects midtones more than highlights/shadows
            let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
            let midtoneMask = 4.0 * luma * (1.0 - luma);
            color = color + noise * midtoneMask;
        } else {
            color = color + noise;
        }
    }

    if (pp.bloomIntensity > 0.0) {
        let bloom = textureSample(bloomTex, sceneSampler, in.uv).rgb;
        // Clamp intensity to prevent overpowering bloom from aggressive game data
        let clampedIntensity = min(pp.bloomIntensity, 1.0);
        let bloomContrib = bloom * clampedIntensity * pp.bloomColor.rgb;
        // Screen blend instead of additive — prevents blown-out whites
        color = 1.0 - (1.0 - color) * (1.0 - bloomContrib * 0.25);
    }

    // The image's noise (postprocess.ps option bits 0x4 / 0x800000000000, read
    // from the shipped xbox_shaders microcode; constants from NgPostProc::
    // CheckNoise). n = map(base uv) * map(top uv), overlay-blended into the
    // bloom-screened colour S. Plain form: per channel, 2nS below 0.5 and
    // 1 - 2(1-n)(1-S) above, mixed in at the intensity. Midtone form: the overlay
    // takes sqrt(n) and branches on the luma L = (0.3,0.59,0.11).S, and its weight
    // is intensity * 6.75 L (1-L)^2, which peaks at 1 for L = 1/3.
    if (pp.noiseMode > 0.5) {
        var baseUv = (in.uv + pp.noiseSeeds.xy) * pp.noiseParams.xy;
        var topUv = (in.uv + pp.noiseSeeds.zw) * pp.noiseParams.xy * pp.noiseParams.z;
        // np-look23 (a), only when the CPU side retiled the map to output rows
        // (NoiseRows.h): every row of a texel samples it d past its centre.
        if (pp.noiseSnapBase > 0.5) {
            let mapH = f32(textureDimensions(noiseTex).y);
            baseUv.y = (floor(baseUv.y * mapH) + 0.5 + (pp.noiseSnapBase - 1.0)) / mapH;
            topUv.y = (floor(topUv.y * mapH) + 0.5 + pp.noiseSnapTop) / mapH;
        }
        let n = textureSample(noiseTex, noiseSampler, baseUv).rgb *
                textureSample(noiseTex, noiseSampler, topUv).rgb;
        let s = clamp(color, vec3f(0.0), vec3f(1.0));
        let intensity = pp.noiseParams.w;
        if (pp.noiseMode < 1.5) {
            let lo = 2.0 * n * s;
            let hi = 1.0 - 2.0 * (1.0 - n) * (1.0 - s);
            color = mix(s, select(hi, lo, s < vec3f(0.5)), intensity);
        } else {
            let r = sqrt(n);
            let luma = dot(s, vec3f(0.3, 0.59, 0.11));
            let lo = 2.0 * r * s;
            let hi = 1.0 - 2.0 * (1.0 - r) * (1.0 - s);
            let k = intensity * 6.75 * luma * (1.0 - luma) * (1.0 - luma);
            color = s + k * (select(hi, lo, luma <= 0.5) - s);
        }
    }

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

void PostProcPass::Init(GpuDevice& gpu) {
    mBloom.Init(gpu);
    SamplerDesc sd{};
    mDefaultSampler = gpu.GetSampler(sd);
    // The noise map: linear, wrapping (CheckNoise: SetTextureFilter(0xd, 1),
    // SetTextureClamp(0xd, 0)), no mip filtering.
    SamplerDesc nd{};
    nd.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    mNoiseSampler = gpu.GetSampler(nd);
}

void PostProcPass::EnsurePipeline(GpuDevice& gpu) {
    if (mPostProcReady) return;
    auto& dev = gpu.Device();

    wgpu::ShaderSourceWGSL src;
    src.code = kPostProcShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    mPostProcShader = dev.CreateShaderModule(&smDesc);

    wgpu::BindGroupLayoutEntry entries[6] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[2].buffer.minBindingSize = sizeof(PostProcUniforms);
    entries[3].binding = 3;
    entries[3].visibility = wgpu::ShaderStage::Fragment;
    entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[4].binding = 4;
    entries[4].visibility = wgpu::ShaderStage::Fragment;
    entries[4].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[4].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[5].binding = 5;
    entries[5].visibility = wgpu::ShaderStage::Fragment;
    entries[5].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 6;
    bglDesc.entries = entries;
    mPostProcBGL = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mPostProcBGL;
    mPostProcPipelineLayout = dev.CreatePipelineLayout(&plDesc);

    wgpu::BufferDescriptor bufDesc{};
    bufDesc.size = sizeof(PostProcUniforms);
    bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mPostProcUniformBuffer = dev.CreateBuffer(&bufDesc);

    wgpu::ColorTargetState ct{};
    ct.format = gpu.SurfaceFormat();
    ct.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = mPostProcShader;
    frag.entryPoint = "fs_postproc";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::RenderPipelineDescriptor pipeDesc{};
    pipeDesc.layout = mPostProcPipelineLayout;
    pipeDesc.vertex.module = mPostProcShader;
    pipeDesc.vertex.entryPoint = "vs_postproc";
    pipeDesc.fragment = &frag;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    mPostProcPipeline = CreateCountedRenderPipeline(dev, &pipeDesc);
    mPostProcReady = true;
}

// Seconds since the previous Run; also advances the noise clock.
float PostProcPass::StepTime() {
    auto now = std::chrono::steady_clock::now();
    if (!mTimeInit) { mLastTime = now; mTimeInit = true; }
    float dt = std::chrono::duration<float>(now - mLastTime).count();
    mLastTime = now;
    mNoiseTime += dt;
    return dt;
}

// Flicker: random brightness modulation between bounds over time.
// Match original engine guard: all three must be positive for flicker to activate
float PostProcPass::StepFlicker(RndPostProc* pp, float dt) {
    const Vector2& flickerMod = pp->GetFlickerModBounds();
    const Vector2& flickerTime = pp->GetFlickerTimeBounds();
    if (flickerTime.x > 0.0f && flickerTime.y > 0.0f && flickerMod.y > 0.0f) {
        mFlickerTimer -= dt;
        if (mFlickerTimer <= 0.0f) {
            // Pick new random target and duration
            // Original: mFlickerMod = 1.0f - RandomFloat(modBounds.x, modBounds.y)
            float t = (float)rand() / (float)RAND_MAX;
            mFlickerTarget = 1.0f - (flickerMod.x + t * (flickerMod.y - flickerMod.x));
            float dur = flickerTime.x + ((float)rand() / (float)RAND_MAX) * (flickerTime.y - flickerTime.x);
            mFlickerTimer = dur > 0.0f ? dur : 0.1f;
        }
        // Lerp toward target
        float rate = dt * 10.0f;
        if (rate > 1.0f) rate = 1.0f;
        mFlickerCurrent += (mFlickerTarget - mFlickerCurrent) * rate;
        return mFlickerCurrent;
    }
    return 1.0f;
}

// Row-major, row-vector 4x4 inverse (Gauss-Jordan, partial pivoting).
static bool Invert4(const double* m, double* out) {
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-300) return false;
        if (piv != c)
            for (int j = 0; j < 8; j++) std::swap(a[c][j], a[piv][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f == 0.0) continue;
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = a[i][j + 4];
    return true;
}

void PostProcPass::SetVelocity(bool ready, float scale, const float viewProj[16]) {
    double cur[16];
    for (int i = 0; i < 16; i++) cur[i] = viewProj[i];
    mVelPending = true;
    mVelReady = false;
    mVelScale = scale;
    // A run of ready frames ending is RndVelocityBuffer::ResetFrame (a camera
    // cut, CamShot::StartAnim): logged, capped, so a capture can be matched
    // against it.
    if (!ready && mVelReadyRun > 0 && mVelCutLogs < 64) {
        mVelCutLogs++;
        fprintf(stderr, "DC3 post: velocity reset after %d ready frames (camera cut), scale %.3f\n",
                mVelReadyRun, scale);
    }
    mVelReadyRun = ready ? mVelReadyRun + 1 : 0;
    double inv[16];
    if (ready && mHavePrevVP && Invert4(cur, inv)) {
        // NDC (x, y, depth, 1) * inverse(VP) = this frame's world (homogeneous);
        // * prevVP = last frame's clip position.
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                double sum = 0.0;
                for (int k = 0; k < 4; k++) sum += inv[i * 4 + k] * mPrevVP[k * 4 + j];
                mVelReproj[i * 4 + j] = (float)sum;
            }
        }
        mVelReady = true;
    }
    std::memcpy(mPrevVP, cur, sizeof(cur));
    mHavePrevVP = true;
}

// np-gfx-post: DC3's retail chain. Modes: rndshape::RetailPostMode (DC3 shape).
template <class P>
void PostProcPass::RunDC3Retail(wgpu::CommandEncoder& encoder, P* pp, int mode,
                                const wgpu::TextureView& sceneView,
                                const wgpu::TextureView& intermediateView,
                                const wgpu::Texture& intermediateTex, int w, int h,
                                const wgpu::TextureView& depthView, uint32_t depthSamples,
                                wgpu::TextureView& frameView, const wgpu::TextureView& spotBeams,
                                const wgpu::TextureView& spotFog, GpuDevice& gpu) {
    // Dependent on P, so the RB3-Wii shape's instantiation compiles nothing.
    if constexpr (rndshape::kDC3RetailPost && sizeof(P) > 0) {
        StepTime();
        // {rnd toggle_all_postprocs} (Rnd::mDisablePostProc): Rnd::DoPostProcess
        // skips DoPost, and DxRnd's FinishPostProcess copies the scene through
        // the option-free postprocess.ps all the same: the identity (mode 2).
        if (rndshape::PostProcDisabled()) mode = 2;
        RetailPostParams rp;
        if (mode != 2) {
            rndshape::FillRetailPost(pp, 1.0f, rp);
            bool midtone = false;
            wgpu::TextureView map;
            if (NoiseConstants(pp, rp.noiseSeeds, rp.noiseParams, rp.noiseSnap, map, midtone,
                               NoiseOutputRows(h, gpu))) {
                rp.noiseMap = map;
                rp.noiseMidtone = midtone ? 1.0f : 0.0f;
            }
        } else {
            rp.dc3 = true;   // the scene through the identity matrix
        }
        if (mode == 3 || mode == 4) rp.debugView = mode - 2;
        if (mode == 5) rp.bloom = false;
        if (sInspectSkip & kSkipBloom) rp.bloom = rp.glare = rp.streak = false;
        if (sInspectSkip & kSkipMotionBlur) rp.motionBlur = false;
        if (mode == 6) rp.debugView = 4;
        if (mode == 7) rp.debugView = 5;
        // NgSpotlightDrawer's beams (WgpuRnd::RunDC3SpotBeams), retail's bit 51.
        if (spotBeams && mode != 2) {
            rp.spotBeams = spotBeams;
            rp.spotFog = spotFog;
            rp.spotScale = mSpotK[0];
            rp.spotBase = mSpotK[1];
            rp.spotSmoke = mSpotK[2];
        }
        // DoVelocity's result for this frame (SetVelocity), retail's bit 42.
        const bool velPending = mVelPending;
        mVelPending = false;
        if (velPending && mVelReady && mode != 2 && !(sInspectSkip & kSkipVelocity)) {
            rp.velocity = true;
            rp.velocityScale = mVelScale;
            std::memcpy(rp.velocityReproj, mVelReproj, sizeof(rp.velocityReproj));
            rp.velocityDepth = depthView;
            rp.velocityDepthSamples = depthSamples;
        }
        // Which post-process grades the frame, once per change: the venue's
        // comes through RndPostProc::Select, which logs nothing. Not on an
        // identity frame (mode 2, toggle_all_postprocs): no proc grades it and
        // rp is unfilled, so the line would print c6=(0 0 0); the change is
        // logged on the next graded frame instead.
        if (pp != mLoggedPostProc && mode != 2) {
            mLoggedPostProc = pp;
            const char* dir = pp->Dir() ? pp->Dir()->Name() : "";
            fprintf(stderr, "DC3 post: %s (%s) bloom I=%.2f thr=%.2f streak=%d glare=%d c6=(%.2f %.2f %.2f) "
                     "xfm=%d vignette=%.2f chroma=%.2f noise=%d motion_blur=%d(%.3f %.3f %.3f %.3f) "
                     "hue_converge=%d(%.3f %.3f %.3f %.3f) velocity=%d\n",
                     pp->Name(), dir, pp->BloomIntensity(), pp->GetBloomThreshold(),
                     (int)pp->GetBloomStreak(), (int)pp->GetBloomGlare(), rp.bloomColor[0],
                     rp.bloomColor[1], rp.bloomColor[2], (int)pp->ColorXfmEnabled(),
                     pp->GetVignetteIntensity(), pp->GetChromaticAberrationOffset(),
                     (int)(bool)rp.noiseMap, (int)rp.motionBlur, rp.motionBlurParams[0],
                     rp.motionBlurParams[1], rp.motionBlurParams[2], rp.motionBlurParams[3],
                     (int)rp.hueConverge, rp.hueParams[0], rp.hueParams[1], rp.hueParams[2],
                     rp.hueParams[3], (int)pp->GetMotionBlurVelocity());
        }
        // Round 10, DC3_LOOK=enhanced: the world grade only (SetWorldGrade),
        // never the identity copy (mode 2) nor a post over the UI.
        if (mWorldGrade && mode != 2) {
            rp.enhPrecision = milo::Look().postPrecision;
            rp.enhFilter = milo::Look().postBloomFilter;
        }
        // A frame drawn at other than the logical size (a render scale, the
        // output's own resolution, or both): bloom from the scene resampled to
        // the logical size, so its targets are the original's 1280x720-based
        // ones (RetailPostParams::renderScale). The copy keeps the scene's
        // alpha, which is the bloom mask: a frame SMALLER than the logical
        // size (a window under 1280x720) is enlarged, not box-filtered, and the
        // present's opaque one-tap copy made every pixel bloom there.
        wgpu::TextureView bloomSrc = intermediateView;
        rp.renderScale = gpu.RenderScaleFor(w);
        if (rp.renderScale != 1.0f && rp.bloom && intermediateTex) {
            const int ow = gpu.LogicalWidth(), oh = gpu.LogicalHeight();
            const wgpu::TextureFormat fmt = intermediateTex.GetFormat();
            if (!mBloomSrcTex || (int)mBloomSrcTex.GetWidth() != ow ||
                (int)mBloomSrcTex.GetHeight() != oh || mBloomSrcTex.GetFormat() != fmt) {
                wgpu::TextureDescriptor td{};
                td.label = "DC3BloomSourceLogicalSize";
                td.size = {(uint32_t)ow, (uint32_t)oh, 1};
                td.format = fmt;
                td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
                mBloomSrcTex = gpu.CreateTexture(td);
                mBloomSrcView = mBloomSrcTex.CreateView();
                mBloomSrcScale.SetBoxFilter(true);
                mBloomSrcScale.SetKeepAlpha(true);
                mBloomSrcScale.SetPassLabel("DC3BloomSourceDownsample");
            }
            mBloomSrcScale.Encode(gpu.Device(), encoder, intermediateTex, mBloomSrcView, fmt, ow, oh);
            bloomSrc = mBloomSrcView;
            rp.bloomSrcW = ow;
            rp.bloomSrcH = oh;
        }
        mRetail.Run(encoder, sceneView, bloomSrc, w, h, frameView, rp, gpu);
    }
}

// Round 10: whether this Run's depth of field takes the enhanced look
// (LookSettings::postDofFilter, the world grade only). A template for the same
// reason as RunDC3Retail: the RB3-Wii flavour links no LookProfile.
template <class P>
static bool EnhancedDof(const P*, bool worldGrade) {
    if constexpr (rndshape::kDC3RetailPost && sizeof(P) > 0) {
        return worldGrade && milo::Look().postDofFilter;
    } else {
        return false;
    }
}
// np-look23 (c): the enhanced depth of field's depth-weighted blur
// (LookSettings::postDofDepthWeight), with EnhancedDof only.
template <class P>
static bool DepthWeightedDof(const P*, bool worldGrade) {
    if constexpr (rndshape::kDC3RetailPost && sizeof(P) > 0) {
        return worldGrade && milo::Look().postDofDepthWeight;
    } else {
        return false;
    }
}
// np-look36: the near field's rule (LookSettings::postDofNearField / Spread),
// with DepthWeightedDof only. Both are decided by adapter class
// (milo::LookDofNearField / LookDofNearSpread).
template <class P>
static bool NearFieldDof(const P*, bool worldGrade, bool spread, milo::LookAdapterClass adapter) {
    if constexpr (rndshape::kDC3RetailPost && sizeof(P) > 0) {
        return worldGrade && (spread ? milo::LookDofNearSpread(milo::Look(), adapter)
                                     : milo::LookDofNearField(milo::Look(), adapter));
    } else {
        return false;
    }
}

// np-look51: the edge rule (LookSettings::postDofEdge), with EnhancedDof only
// (DofPass ignores it otherwise), decided by adapter class (milo::LookDofEdge).
template <class P>
static bool EdgeDof(const P*, bool worldGrade, milo::LookAdapterClass adapter) {
    if constexpr (rndshape::kDC3RetailPost && sizeof(P) > 0) {
        return worldGrade && milo::LookDofEdge(milo::Look(), adapter);
    } else {
        return false;
    }
}

void PostProcPass::Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& intermediateView,
                       const wgpu::Texture& intermediateTex, int intermediateW, int intermediateH,
                       const wgpu::TextureView& depthView, uint32_t depthSamples,
                       wgpu::TextureView& frameView, wgpu::TextureView& blackTexView,
                       GpuDevice& gpu, const wgpu::TextureView& colorSampleView) {
    EnsurePipeline(gpu);

    // The beams are this frame's only, whichever composite runs.
    wgpu::TextureView spotBeams = mSpotBeams, spotFog = mSpotFog;
    mSpotBeams = nullptr;
    mSpotFog = nullptr;

    RndPostProc* pp = RndPostProc::Current();
    // DC3: the post-process Rnd::DoPostProcess runs (its blacklight override,
    // else its override, else the selected one).
    if constexpr (rndshape::kDC3RetailPost) pp = rndshape::ActivePostProc();
    if (!pp) return;

    // Depth of field first. As in retail, where the blur is mixed in by the
    // post-process shader and bloom reads the pre-process texture, the
    // depth-of-field image replaces the scene in the composite only; bloom
    // still reads the sharp scene.
    // DC3: NgDOFProc is one of the registered post processors, so
    // {rnd toggle_all_postprocs} (Rnd::DoPostProcess skipping every DoPost)
    // turns the depth of field off with the rest of the chain.
    // rb3-xenon has no {rnd native_post_skip}: MILO_RB3_POST_SKIP=<mask> sets
    // the same inspection mask once (kRetailPostChain consumers only).
    if constexpr (rndshape::kRetailPostChain) {
        static const bool sEnvRead = [] {
            if (const char* e = getenv("MILO_RB3_POST_SKIP")) {
                sInspectSkip = (unsigned)strtoul(e, nullptr, 0);
                fprintf(stderr, "RB3 post: inspection skip mask 0x%x\n", sInspectSkip);
            }
            return true;
        }();
        (void)sEnvRead;
    }
    wgpu::TextureView sceneView = intermediateView;
    bool dofRuns = true;
    if constexpr (rndshape::kDC3RetailPost) dofRuns = !rndshape::PostProcDisabled();
    if (sInspectSkip & kSkipDof) dofRuns = false;
    if (dofRuns && mDof.Run(encoder, intermediateView, intermediateTex.GetFormat(), depthView,
                            depthSamples, intermediateW, intermediateH, gpu, EnhancedDof(pp, mWorldGrade),
                            DepthWeightedDof(pp, mWorldGrade),
                            NearFieldDof(pp, mWorldGrade, false, gpu.LookClass()),
                            NearFieldDof(pp, mWorldGrade, true, gpu.LookClass()),
                            EdgeDof(pp, mWorldGrade, gpu.LookClass()), colorSampleView)) {
        sceneView = mDof.OutputView();
    }

    if constexpr (rndshape::kRetailPostChain) {
        if (const int mode = rndshape::RetailPostMode()) {
            float dt = StepTime();
            RetailPostParams rp;
            // StepFlicker only when the chain grades (as before: raw skips it).
            const float flicker = mode != 2 ? StepFlicker(pp, dt) : 1.0f;
            FillRB3Chain(pp, mode, flicker, NoiseOutputRows(intermediateH, gpu), rp);
            rp.time = mNoiseTime;
            if (spotBeams && mode != 2) {
                rp.spotBeams = spotBeams;
                rp.spotFog = spotFog;
                rp.spotScale = mSpotK[0];
                rp.spotBase = mSpotK[1];
                rp.spotSmoke = mSpotK[2];
            }
            mRetail.Run(encoder, sceneView, intermediateView, intermediateW, intermediateH, frameView, rp,
                        gpu);
            return;
        }
    }

    // np-gfx-post: DC3's retail chain (DC3_RETAIL_POST=0 keeps the composite below).
    if constexpr (rndshape::kDC3RetailPost) {
        if (const int mode = rndshape::RetailPostMode()) {
            RunDC3Retail(encoder, pp, mode, sceneView, intermediateView, intermediateTex,
                         intermediateW, intermediateH,
                         depthView, depthSamples, frameView, spotBeams, spotFog, gpu);
            return;
        }
    }

    // Run bloom if active — clamp intensity and raise threshold floor
    float bloomIntensity = std::min(pp->GetBloomIntensity(), 1.0f);
    float bloomThreshold = std::max(pp->GetBloomThreshold(), 0.7f);
    if (bloomIntensity > 0.0f) {
        mBloom.Run(encoder, intermediateView, intermediateW, intermediateH,
                   bloomIntensity, bloomThreshold, pp->GetBloomColor(), gpu);
    }

    // Fill uniforms
    PostProcUniforms uni{};
    const RndColorXfm& cxfm = pp->GetColorXfm();
    uni.contrast = cxfm.mContrast;
    uni.brightness = cxfm.mBrightness;
    uni.saturation = cxfm.mSaturation;
    uni.vignetteIntensity = pp->GetVignetteIntensity();
    const Hmx::Color& vc = pp->GetVignetteColor();
    uni.vignetteColor[0] = vc.red;
    uni.vignetteColor[1] = vc.green;
    uni.vignetteColor[2] = vc.blue;
    uni.vignetteColor[3] = vc.alpha;
    uni.chromaticOffset = pp->GetChromaticAberrationOffset();
    uni.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
    uni.posterLevels = pp->GetPosterLevels();
    uni.posterMin = pp->GetPosterMin();

    uni.levelInLo[0] = cxfm.mLevelInLo.red;   uni.levelInLo[1] = cxfm.mLevelInLo.green;
    uni.levelInLo[2] = cxfm.mLevelInLo.blue;   uni.levelInLo[3] = 0;
    uni.levelInHi[0] = cxfm.mLevelInHi.red;   uni.levelInHi[1] = cxfm.mLevelInHi.green;
    uni.levelInHi[2] = cxfm.mLevelInHi.blue;   uni.levelInHi[3] = 1;
    uni.levelOutLo[0] = cxfm.mLevelOutLo.red; uni.levelOutLo[1] = cxfm.mLevelOutLo.green;
    uni.levelOutLo[2] = cxfm.mLevelOutLo.blue; uni.levelOutLo[3] = 0;
    uni.levelOutHi[0] = cxfm.mLevelOutHi.red; uni.levelOutHi[1] = cxfm.mLevelOutHi.green;
    uni.levelOutHi[2] = cxfm.mLevelOutHi.blue; uni.levelOutHi[3] = 1;

    uni.bloomIntensity = bloomIntensity;
    const Hmx::Color& bc = pp->GetBloomColor();
    uni.bloomColor[0] = bc.red;
    uni.bloomColor[1] = bc.green;
    uni.bloomColor[2] = bc.blue;
    uni.bloomColor[3] = 1.0f;

    // Time tracking for noise animation
    float dt = StepTime();
    uni.time = mNoiseTime;

    // Noise/grain
    wgpu::TextureView noiseView = blackTexView;
    rndshape::PostProcNoise noise;
    if (rndshape::PostProcNoiseMap(pp, noise)) {
        // The map path replaces the procedural grain outright.
        uni.noiseIntensity = 0.0f;
        wgpu::TextureView mapView;
        bool midtone = false;
        float snap[3];
        if (NoiseConstants(pp, uni.noiseSeeds, uni.noiseParams, snap, mapView, midtone,
                           NoiseOutputRows(intermediateH, gpu))) {
            noiseView = mapView;
            uni.noiseMode = midtone ? 2.0f : 1.0f;
            uni.noiseSnapBase = snap[0] > 0.5f ? 1.0f + snap[1] : 0.0f;
            uni.noiseSnapTop = snap[2];
        }
    } else {
        uni.noiseIntensity = rndshape::PostProcGrain(pp);
    }
    uni.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;

    uni.flickerMul = StepFlicker(pp, dt);

    gpu.Queue().WriteBuffer(mPostProcUniformBuffer, 0, &uni, sizeof(uni));

    wgpu::TextureView bloomView = (bloomIntensity > 0.0f && mBloom.HasOutput())
        ? mBloom.OutputView() : blackTexView;

    wgpu::BindGroupEntry bgEntries[6] = {};
    bgEntries[0].binding = 0;
    bgEntries[0].textureView = sceneView;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = mDefaultSampler;
    bgEntries[2].binding = 2;
    bgEntries[2].buffer = mPostProcUniformBuffer;
    bgEntries[2].size = sizeof(PostProcUniforms);
    bgEntries[3].binding = 3;
    bgEntries[3].textureView = bloomView;
    bgEntries[4].binding = 4;
    bgEntries[4].textureView = noiseView;
    bgEntries[5].binding = 5;
    bgEntries[5].sampler = mNoiseSampler;

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mPostProcBGL;
    bgDesc.entryCount = 6;
    bgDesc.entries = bgEntries;
    wgpu::BindGroup bg = mBindGroups ? mBindGroups->GetOrCreate(gpu.Device(), bgDesc)
                                     : CreateCountedBindGroup(gpu.Device(), &bgDesc);

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = frameView;
    colorAtt.loadOp = wgpu::LoadOp::Clear;
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "PostProcComposite";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;

    MILO_GPU_TIME_PASS(rpDesc);
    auto pass = encoder.BeginRenderPass(&rpDesc);
    pass.SetPipeline(mPostProcPipeline);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

// The rows a buffer `h` rows tall leaves the renderer at: the post-process
// buffers are drawn at the frame's drawn size (WindowHeight), which a render
// scale box-filters down to OutputHeight. 1x: h. 1920x1080 at scale 2:
// 2160 * 1080 / 2160 = 1080, the same rows as 1x.
int PostProcPass::NoiseOutputRows(int h, const GpuDevice& gpu) {
    const int drawn = gpu.WindowHeight();
    if (drawn <= 0) return h;
    return (int)std::lround((double)h * gpu.OutputHeight() / drawn);
}

void PostProcPass::FillRB3Chain(RndPostProc* pp, int mode, float flicker, int outputRows,
                                RetailPostParams& rp) {
    rp = RetailPostParams();
    if (!pp) return;
    if (mode != 2) {
        rndshape::FillRetailPost(pp, flicker, rp);
        // NgPostProc::CheckNoise (band.exe TU5 fn_82B89100), for a consumer
        // whose RndPostProc exposes the noise map (rndshape::PostProcNoiseMap;
        // rb3-xenon): the map, its seeds and scales, as the DC3 chain takes
        // them. Without the accessors (the RB3-Wii shape) nothing changes.
        bool midtone = false;
        wgpu::TextureView map;
        if (!(sInspectSkip & kSkipNoise) &&
            NoiseConstants(pp, rp.noiseSeeds, rp.noiseParams, rp.noiseSnap, map, midtone, outputRows)) {
            rp.noiseMap = map;
            rp.noiseMidtone = midtone ? 1.0f : 0.0f;
        }
    }
    if (mode == 3 || mode == 4) rp.debugView = mode - 2;
    if (mode == 5) rp.bloom = false;
    if (mode == 6) rp.debugView = 3;
    if (sInspectSkip & kSkipBloom) rp.bloom = false;
    if (sInspectSkip & kSkipColorXfm) {
        const float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        std::memcpy(rp.xfm, id, sizeof(id));
    }
}

bool PostProcPass::NoiseRowsSnapped(const milo::LookSettings& look) {
    return rndshape::kNoiseSnapsToOutputRows && look.noiseSnapsToOutputRows;
}

void PostProcPass::SnapNoiseConstants(int outputRows, int mapH, float seeds[4], float params[4],
                                      float snap[3]) {
    snap[0] = snap[1] = snap[2] = 0.0f;
    if (!NoiseRowsSnapped(milo::Look())) return;
    ApplyNoiseRowSnap(mNoiseRowSnap.Get(outputRows, mapH, std::fabs((double)params[1]),
                                        std::fabs((double)params[2])),
                      seeds, params, snap);
}

bool PostProcPass::NoiseConstants(RndPostProc* pp, float seeds[4], float params[4],
                                  float snap[3], wgpu::TextureView& map, bool& midtone,
                                  int outputRows) {
    snap[0] = snap[1] = snap[2] = 0.0f;
    rndshape::PostProcNoise noise;
    if (!rndshape::PostProcNoiseMap(pp, noise)) return false;
    // NgPostProc::CheckNoise: no noise unless the intensity is non-zero AND
    // there is a map.
    // GetGpuTexView answers only for a texture something has uploaded, and
    // materials upload theirs as they bind them (MaterialSetup's ResolveMap).
    // noise_mono.tex is bound by nothing but this pass, so without the presync
    // here it never reached the GPU in game: every "awesome" frame ran with
    // its noise silently off, -30% luma against the image (np-gfx round 4,
    // Xenia). Once resident, PresyncBitmap is a lookup and a fingerprint.
    map = wgpu::TextureView();
    if (noise.intensity != 0.0f && noise.map) {
        if (!noise.map->IsRenderTarget()) noise.map->PresyncBitmap();
        map = GetGpuTexView(noise.map);
    }
    if (!map) return false;
    midtone = noise.midtone;
    // Stationary noise keeps the seed pair NgPostProc::Select rolled
    // (mRandomSeed, re-rolled here when the selected post-process changes)
    // and samples both layers at the base scale; otherwise four fresh seeds
    // every frame and the top layer at mNoiseTopScale.
    if (pp != mNoiseSeedOwner) {
        mNoiseSeedOwner = pp;
        mStationarySeed[0] = (float)rand() / (float)RAND_MAX;
        mStationarySeed[1] = (float)rand() / (float)RAND_MAX;
    }
    if (noise.stationary) {
        seeds[0] = mStationarySeed[0];
        seeds[1] = mStationarySeed[1];
        seeds[2] = mStationarySeed[0];
        seeds[3] = mStationarySeed[1];
    } else {
        for (int i = 0; i < 4; i++) seeds[i] = (float)rand() / (float)RAND_MAX;
    }
    params[0] = noise.baseScale[0];
    params[1] = noise.baseScale[1];
    params[2] = noise.stationary ? 1.0f : noise.topScale;
    params[3] = noise.intensity;
    // np-look23 (a): one texel per whole number of output rows, read at one
    // place in it (NoiseRows.h). The enhanced look's, on a shape with the
    // capability; otherwise every constant as authored. The derived values
    // are memoised on (rows, map height, base, top).
    SnapNoiseConstants(outputRows, noise.map->Height(), seeds, params, snap);
    return true;
}

void PostProcPass::Terminate() {
    mBloom.Terminate();
    mRetail.Terminate();
    mDof.Terminate();

    mPostProcShader = nullptr;
    mPostProcBGL = nullptr;
    mPostProcPipelineLayout = nullptr;
    mPostProcPipeline = nullptr;
    mPostProcUniformBuffer = nullptr;
    mDefaultSampler = nullptr;
    mNoiseSampler = nullptr;
    mNoiseSeedOwner = nullptr;
    mLoggedPostProc = nullptr;
    mPostProcReady = false;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* PostProcPassWgslSource() { return kPostProcShaderSource; }
