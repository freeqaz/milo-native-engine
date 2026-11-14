#include "gfx/PostProcPass.h"
#include "gfx/GpuDevice.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"
#include "platform/rndshape/RndShape.h"
#include "gfx/RB3RetailPost.h"
#include "platform/TexGpu.h"
#include <cstdio>
#include <cmath>
#include <utility>

#include <cstring>
#include <cstdlib>

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
    float _pad1;
    float _pad2;
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
    _pad1: f32,
    _pad2: f32,
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
        let baseUv = (in.uv + pp.noiseSeeds.xy) * pp.noiseParams.xy;
        let topUv = (in.uv + pp.noiseSeeds.zw) * pp.noiseParams.xy * pp.noiseParams.z;
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

    mPostProcPipeline = dev.CreateRenderPipeline(&pipeDesc);
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
                                const wgpu::TextureView& intermediateView, int w, int h,
                                const wgpu::TextureView& depthView, uint32_t depthSamples,
                                wgpu::TextureView& frameView, GpuDevice& gpu) {
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
            if (NoiseConstants(pp, rp.noiseSeeds, rp.noiseParams, map, midtone)) {
                rp.noiseMap = map;
                rp.noiseMidtone = midtone ? 1.0f : 0.0f;
            }
        } else {
            rp.dc3 = true;   // the scene through the identity matrix
        }
        if (mode == 3 || mode == 4) rp.debugView = mode - 2;
        if (mode == 5) rp.bloom = false;
        if (mode == 6) rp.debugView = 4;
        if (mode == 7) rp.debugView = 5;
        // DoVelocity's result for this frame (SetVelocity), retail's bit 42.
        const bool velPending = mVelPending;
        mVelPending = false;
        if (velPending && mVelReady && mode != 2) {
            rp.velocity = true;
            rp.velocityScale = mVelScale;
            std::memcpy(rp.velocityReproj, mVelReproj, sizeof(rp.velocityReproj));
            rp.velocityDepth = depthView;
            rp.velocityDepthSamples = depthSamples;
        }
        // Which post-process grades the frame, once per change: the venue's
        // comes through RndPostProc::Select, which logs nothing.
        if (pp != mLoggedPostProc) {
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
        mRetail.Run(encoder, sceneView, intermediateView, w, h, frameView, rp, gpu);
    }
}

void PostProcPass::Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& intermediateView,
                       const wgpu::Texture& intermediateTex, int intermediateW, int intermediateH,
                       const wgpu::TextureView& depthView, uint32_t depthSamples,
                       wgpu::TextureView& frameView, wgpu::TextureView& blackTexView,
                       GpuDevice& gpu) {
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
    wgpu::TextureView sceneView = intermediateView;
    if (mDof.Run(encoder, intermediateView, intermediateTex.GetFormat(), depthView, depthSamples,
                 intermediateW, intermediateH, gpu)) {
        sceneView = mDof.OutputView();
    }

    if constexpr (rndshape::kRetailPostChain) {
        if (const int mode = rndshape::RetailPostMode()) {
            float dt = StepTime();
            RetailPostParams rp;
            if (mode != 2) rndshape::FillRetailPost(pp, StepFlicker(pp, dt), rp);
            if (mode == 3 || mode == 4) rp.debugView = mode - 2;
            if (mode == 5) rp.bloom = false;
            if (mode == 6) rp.debugView = 3;
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
            RunDC3Retail(encoder, pp, mode, sceneView, intermediateView, intermediateW, intermediateH,
                         depthView, depthSamples, frameView, gpu);
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
        if (NoiseConstants(pp, uni.noiseSeeds, uni.noiseParams, mapView, midtone)) {
            noiseView = mapView;
            uni.noiseMode = midtone ? 2.0f : 1.0f;
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
                                     : gpu.Device().CreateBindGroup(&bgDesc);

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = frameView;
    colorAtt.loadOp = wgpu::LoadOp::Clear;
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;

    auto pass = encoder.BeginRenderPass(&rpDesc);
    pass.SetPipeline(mPostProcPipeline);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

bool PostProcPass::NoiseConstants(RndPostProc* pp, float seeds[4], float params[4],
                                  wgpu::TextureView& map, bool& midtone) {
    rndshape::PostProcNoise noise;
    if (!rndshape::PostProcNoiseMap(pp, noise)) return false;
    // NgPostProc::CheckNoise: no noise unless the intensity is non-zero AND
    // there is a map.
    map = (noise.intensity != 0.0f && noise.map) ? GetGpuTexView(noise.map) : wgpu::TextureView();
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
