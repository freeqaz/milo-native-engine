#include "gfx/PostProcPass.h"
#include "gfx/GpuDevice.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"

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
    float _pad0;
    float _pad1;
    float _pad2;
};
static_assert(sizeof(PostProcUniforms) == 160, "PostProcUniforms must be 160 bytes");

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
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var sceneSampler: sampler;
@group(0) @binding(2) var<uniform> pp: PostProcUB;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;

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

    // Noise/grain: procedural hash-based noise overlay
    if (pp.noiseIntensity != 0.0) {
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

    return vec4f(clamp(color, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";

void PostProcPass::Init(GpuDevice& gpu) {
    mBloom.Init(gpu);
    SamplerDesc sd{};
    mDefaultSampler = gpu.GetSampler(sd);
}

void PostProcPass::EnsurePipeline(GpuDevice& gpu) {
    if (mPostProcReady) return;
    auto& dev = gpu.Device();

    wgpu::ShaderSourceWGSL src;
    src.code = kPostProcShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    mPostProcShader = dev.CreateShaderModule(&smDesc);

    wgpu::BindGroupLayoutEntry entries[4] = {};
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

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 4;
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

void PostProcPass::Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
                       wgpu::Texture& intermediateTex, int intermediateW, int intermediateH,
                       wgpu::TextureView& depthView, wgpu::TextureView& frameView,
                       wgpu::TextureView& blackTexView, GpuDevice& gpu) {
    EnsurePipeline(gpu);

    RndPostProc* pp = RndPostProc::Current();
    if (!pp) return;

    // Run DOF before bloom/composite
    mDof.Run(encoder, intermediateView, intermediateTex, depthView,
             intermediateW, intermediateH, gpu);

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
    auto now = std::chrono::steady_clock::now();
    if (!mTimeInit) { mLastTime = now; mTimeInit = true; }
    float dt = std::chrono::duration<float>(now - mLastTime).count();
    mLastTime = now;
    static float sTime = 0.0f;
    sTime += dt;
    uni.time = sTime;

    // Noise/grain
    uni.noiseIntensity = pp->GetNoiseIntensity();
    uni.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;

    // Flicker: random brightness modulation between bounds over time
    // Match original engine guard: all three must be positive for flicker to activate
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
        uni.flickerMul = mFlickerCurrent;
    } else {
        uni.flickerMul = 1.0f;
    }

    gpu.Queue().WriteBuffer(mPostProcUniformBuffer, 0, &uni, sizeof(uni));

    wgpu::TextureView bloomView = (bloomIntensity > 0.0f && mBloom.HasOutput())
        ? mBloom.OutputView() : blackTexView;

    wgpu::BindGroupEntry bgEntries[4] = {};
    bgEntries[0].binding = 0;
    bgEntries[0].textureView = intermediateView;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = mDefaultSampler;
    bgEntries[2].binding = 2;
    bgEntries[2].buffer = mPostProcUniformBuffer;
    bgEntries[2].size = sizeof(PostProcUniforms);
    bgEntries[3].binding = 3;
    bgEntries[3].textureView = bloomView;

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mPostProcBGL;
    bgDesc.entryCount = 4;
    bgDesc.entries = bgEntries;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgDesc);

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

void PostProcPass::Terminate() {
    mBloom.Terminate();
    mDof.Terminate();

    mPostProcShader = nullptr;
    mPostProcBGL = nullptr;
    mPostProcPipelineLayout = nullptr;
    mPostProcPipeline = nullptr;
    mPostProcUniformBuffer = nullptr;
    mDefaultSampler = nullptr;
    mPostProcReady = false;
}
