#include "gfx/BloomPass.h"
#include "gfx/GpuDevice.h"

#include <algorithm>
#include <cstring>

struct BloomUniforms {
    float threshold;
    float texelSizeX;
    float texelSizeY;
    float intensity;
};
static_assert(sizeof(BloomUniforms) == 16, "BloomUniforms must be 16 bytes");

static const char* kBloomShaderSource = R"WGSL(
struct BloomUB {
    threshold: f32,
    texelSizeX: f32,
    texelSizeY: f32,
    intensity: f32,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var srcSampler: sampler;
@group(0) @binding(2) var<uniform> bloom: BloomUB;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_bloom(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_bloom_threshold(in: VOut) -> @location(0) vec4f {
    let color = textureSample(srcTex, srcSampler, in.uv).rgb;
    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    // Soft knee: smooth transition around threshold instead of hard cutoff
    let knee = bloom.threshold * 0.5;
    let soft = luma - bloom.threshold + knee;
    let contribution = clamp(soft / (2.0 * knee + 0.0001), 0.0, 1.0);
    let weight = contribution * contribution; // quadratic falloff for softer look
    return vec4f(color * weight, 1.0);
}

@fragment fn fs_bloom_blur_h(in: VOut) -> @location(0) vec4f {
    let ts = vec2f(bloom.texelSizeX, 0.0);
    var color = textureSample(srcTex, srcSampler, in.uv).rgb * 0.2270270270;
    color += textureSample(srcTex, srcSampler, in.uv + ts * 1.3846153846).rgb * 0.3162162162;
    color += textureSample(srcTex, srcSampler, in.uv - ts * 1.3846153846).rgb * 0.3162162162;
    color += textureSample(srcTex, srcSampler, in.uv + ts * 3.2307692308).rgb * 0.0702702703;
    color += textureSample(srcTex, srcSampler, in.uv - ts * 3.2307692308).rgb * 0.0702702703;
    return vec4f(color, 1.0);
}

@fragment fn fs_bloom_blur_v(in: VOut) -> @location(0) vec4f {
    let ts = vec2f(0.0, bloom.texelSizeY);
    var color = textureSample(srcTex, srcSampler, in.uv).rgb * 0.2270270270;
    color += textureSample(srcTex, srcSampler, in.uv + ts * 1.3846153846).rgb * 0.3162162162;
    color += textureSample(srcTex, srcSampler, in.uv - ts * 1.3846153846).rgb * 0.3162162162;
    color += textureSample(srcTex, srcSampler, in.uv + ts * 3.2307692308).rgb * 0.0702702703;
    color += textureSample(srcTex, srcSampler, in.uv - ts * 3.2307692308).rgb * 0.0702702703;
    return vec4f(color, 1.0);
}

@fragment fn fs_bloom_upsample(in: VOut) -> @location(0) vec4f {
    // 3x3 tent filter for smoother upsampling (less boxy artifacts)
    let tx = bloom.texelSizeX;
    let ty = bloom.texelSizeY;
    var color = textureSample(srcTex, srcSampler, in.uv).rgb * 4.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,  0.0)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,  0.0)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( 0.0,  ty)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( 0.0, -ty)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,   ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,   ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,  -ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,  -ty)).rgb;
    // Divide by 16 for tent weights, then attenuate to prevent mip accumulation
    return vec4f(color * (1.0 / 16.0) * bloom.intensity, 1.0);
}

@fragment fn fs_bloom_downsample(in: VOut) -> @location(0) vec4f {
    // 2x2 bilinear downsample with proper texel offsets from *source* resolution
    let tx = bloom.texelSizeX;  // source texel size
    let ty = bloom.texelSizeY;
    var color = textureSample(srcTex, srcSampler, in.uv).rgb * 4.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,  0.0)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,  0.0)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( 0.0,  ty)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( 0.0, -ty)).rgb * 2.0;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,   ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,   ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f( tx,  -ty)).rgb;
    color += textureSample(srcTex, srcSampler, in.uv + vec2f(-tx,  -ty)).rgb;
    return vec4f(color * (1.0 / 16.0), 1.0);
}
)WGSL";

void BloomPass::Init(GpuDevice& gpu) {
    SamplerDesc sd{};
    mDefaultSampler = gpu.GetSampler(sd);
}

void BloomPass::EnsurePipelines(GpuDevice& gpu) {
    if (mBloomReady) return;
    auto& dev = gpu.Device();

    wgpu::ShaderSourceWGSL src;
    src.code = kBloomShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    mBloomShader = dev.CreateShaderModule(&smDesc);

    wgpu::BindGroupLayoutEntry entries[3] = {};
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
    entries[2].buffer.minBindingSize = sizeof(BloomUniforms);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    mBloomBGL = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mBloomBGL;
    mBloomPipelineLayout = dev.CreatePipelineLayout(&plDesc);

    wgpu::BufferDescriptor bufDesc{};
    bufDesc.size = sizeof(BloomUniforms);
    bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mBloomUniformBuffer = dev.CreateBuffer(&bufDesc);

    auto makePipeline = [&](const char* fsEntry, bool additiveBlend) -> wgpu::RenderPipeline {
        wgpu::BlendState blend{};
        wgpu::ColorTargetState ct{};
        ct.format = gpu.SurfaceFormat();
        if (additiveBlend) {
            blend.color.operation = wgpu::BlendOperation::Add;
            blend.color.srcFactor = wgpu::BlendFactor::One;
            blend.color.dstFactor = wgpu::BlendFactor::One;
            blend.alpha.operation = wgpu::BlendOperation::Add;
            blend.alpha.srcFactor = wgpu::BlendFactor::One;
            blend.alpha.dstFactor = wgpu::BlendFactor::Zero;
            ct.blend = &blend;
        }
        ct.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState frag{};
        frag.module = mBloomShader;
        frag.entryPoint = fsEntry;
        frag.targetCount = 1;
        frag.targets = &ct;

        wgpu::RenderPipelineDescriptor pipeDesc{};
        pipeDesc.layout = mBloomPipelineLayout;
        pipeDesc.vertex.module = mBloomShader;
        pipeDesc.vertex.entryPoint = "vs_bloom";
        pipeDesc.fragment = &frag;
        pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return dev.CreateRenderPipeline(&pipeDesc);
    };

    mBloomThresholdPipeline = makePipeline("fs_bloom_threshold", false);
    mBloomBlurHPipeline = makePipeline("fs_bloom_blur_h", false);
    mBloomBlurVPipeline = makePipeline("fs_bloom_blur_v", false);
    mBloomDownsamplePipeline = makePipeline("fs_bloom_downsample", false);
    mBloomUpsamplePipeline = makePipeline("fs_bloom_upsample", true);

    mBloomReady = true;
}

void BloomPass::EnsureTextures(int sceneW, int sceneH, GpuDevice& gpu) {
    auto& dev = gpu.Device();
    for (int i = 0; i < kBloomMips; i++) {
        int w = std::max(1, sceneW >> (i + 1));
        int h = std::max(1, sceneH >> (i + 1));
        if (mBloomWidth[i] == w && mBloomHeight[i] == h && mBloomTex[i])
            continue;

        wgpu::TextureDescriptor desc{};
        desc.size = {(uint32_t)w, (uint32_t)h, 1};
        desc.format = gpu.SurfaceFormat();
        desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        desc.mipLevelCount = 1;

        mBloomTex[i] = dev.CreateTexture(&desc);
        mBloomView[i] = mBloomTex[i].CreateView();
        mBloomTempTex[i] = dev.CreateTexture(&desc);
        mBloomTempView[i] = mBloomTempTex[i].CreateView();
        mBloomWidth[i] = w;
        mBloomHeight[i] = h;
    }
}

void BloomPass::Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
                    int sceneW, int sceneH, float intensity, float threshold,
                    const Hmx::Color& tint, GpuDevice& gpu) {
    EnsurePipelines(gpu);
    EnsureTextures(sceneW, sceneH, gpu);

    auto& queue = gpu.Queue();
    auto& dev = gpu.Device();

    auto bloomPass = [&](wgpu::TextureView& srcView, wgpu::TextureView& dstView,
                         wgpu::RenderPipeline& pipeline, int targetW, int targetH,
                         float param = 0.0f) {
        BloomUniforms uni{};
        uni.threshold = param;
        uni.texelSizeX = 1.0f / targetW;
        uni.texelSizeY = 1.0f / targetH;
        uni.intensity = intensity;
        queue.WriteBuffer(mBloomUniformBuffer, 0, &uni, sizeof(uni));

        wgpu::BindGroupEntry bgEntries[3] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].textureView = srcView;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = mDefaultSampler;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = mBloomUniformBuffer;
        bgEntries[2].size = sizeof(BloomUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = mBloomBGL;
        bgDesc.entryCount = 3;
        bgDesc.entries = bgEntries;
        wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = dstView;
        colorAtt.loadOp = wgpu::LoadOp::Clear;
        colorAtt.storeOp = wgpu::StoreOp::Store;
        colorAtt.clearValue = {0, 0, 0, 1};

        wgpu::RenderPassDescriptor rpDesc{};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;

        auto pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    };

    auto upsamplePass = [&](wgpu::TextureView& srcView, wgpu::TextureView& dstView,
                            int targetW, int targetH, int srcW, int srcH,
                            float blendWeight) {
        BloomUniforms uni{};
        uni.threshold = 0;
        uni.texelSizeX = 1.0f / srcW;
        uni.texelSizeY = 1.0f / srcH;
        uni.intensity = blendWeight;
        queue.WriteBuffer(mBloomUniformBuffer, 0, &uni, sizeof(uni));

        wgpu::BindGroupEntry bgEntries[3] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].textureView = srcView;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = mDefaultSampler;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = mBloomUniformBuffer;
        bgEntries[2].size = sizeof(BloomUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = mBloomBGL;
        bgDesc.entryCount = 3;
        bgDesc.entries = bgEntries;
        wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = dstView;
        colorAtt.loadOp = wgpu::LoadOp::Load;
        colorAtt.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor rpDesc{};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;

        auto pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetPipeline(mBloomUpsamplePipeline);
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    };

    // 1. Threshold
    bloomPass(intermediateView, mBloomView[0], mBloomThresholdPipeline,
              mBloomWidth[0], mBloomHeight[0], threshold);

    // 2. Blur and downsample
    for (int i = 0; i < kBloomMips; i++) {
        bloomPass(mBloomView[i], mBloomTempView[i], mBloomBlurHPipeline,
                  mBloomWidth[i], mBloomHeight[i]);
        bloomPass(mBloomTempView[i], mBloomView[i], mBloomBlurVPipeline,
                  mBloomWidth[i], mBloomHeight[i]);
        if (i + 1 < kBloomMips) {
            // Downsample with source texel sizes for correct filter coverage
            BloomUniforms uni{};
            uni.threshold = 0;
            uni.texelSizeX = 1.0f / mBloomWidth[i];   // source texel size
            uni.texelSizeY = 1.0f / mBloomHeight[i];
            uni.intensity = 0;
            queue.WriteBuffer(mBloomUniformBuffer, 0, &uni, sizeof(uni));

            wgpu::BindGroupEntry bgEntries[3] = {};
            bgEntries[0].binding = 0;
            bgEntries[0].textureView = mBloomView[i];
            bgEntries[1].binding = 1;
            bgEntries[1].sampler = mDefaultSampler;
            bgEntries[2].binding = 2;
            bgEntries[2].buffer = mBloomUniformBuffer;
            bgEntries[2].size = sizeof(BloomUniforms);

            wgpu::BindGroupDescriptor bgDesc{};
            bgDesc.layout = mBloomBGL;
            bgDesc.entryCount = 3;
            bgDesc.entries = bgEntries;
            wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

            wgpu::RenderPassColorAttachment colorAtt{};
            colorAtt.view = mBloomView[i + 1];
            colorAtt.loadOp = wgpu::LoadOp::Clear;
            colorAtt.storeOp = wgpu::StoreOp::Store;
            colorAtt.clearValue = {0, 0, 0, 1};

            wgpu::RenderPassDescriptor rpDesc{};
            rpDesc.colorAttachmentCount = 1;
            rpDesc.colorAttachments = &colorAtt;

            auto pass = encoder.BeginRenderPass(&rpDesc);
            pass.SetPipeline(mBloomDownsamplePipeline);
            pass.SetBindGroup(0, bg);
            pass.Draw(3);
            pass.End();
        }
    }

    // 3. Upsample chain — each coarser mip contributes less
    static constexpr float kMipWeights[kBloomMips] = {0.8f, 0.5f, 0.3f, 0.2f};
    for (int i = kBloomMips - 2; i >= 0; i--) {
        upsamplePass(mBloomView[i + 1], mBloomView[i],
                      mBloomWidth[i], mBloomHeight[i],
                      mBloomWidth[i + 1], mBloomHeight[i + 1],
                      kMipWeights[i + 1]);
    }
}

void BloomPass::Terminate() {
    for (int i = 0; i < kBloomMips; i++) {
        mBloomTex[i] = nullptr;
        mBloomView[i] = nullptr;
        mBloomTempTex[i] = nullptr;
        mBloomTempView[i] = nullptr;
    }
    mBloomShader = nullptr;
    mBloomBGL = nullptr;
    mBloomPipelineLayout = nullptr;
    mBloomThresholdPipeline = nullptr;
    mBloomBlurHPipeline = nullptr;
    mBloomBlurVPipeline = nullptr;
    mBloomDownsamplePipeline = nullptr;
    mBloomUpsamplePipeline = nullptr;
    mBloomUniformBuffer = nullptr;
    mDefaultSampler = nullptr;
    mBloomReady = false;
}
