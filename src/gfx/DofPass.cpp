#include "gfx/DofPass.h"
#include "gfx/GpuDevice.h"
#include "rndobj/Cam.h"
#include "rndobj/PostProc.h"

#include <algorithm>
#include <cstring>

struct DofUniforms {
    float focalPlane;
    float blurDepth;
    float maxBlur;
    float minBlur;
    float texelSizeX;
    float texelSizeY;
    float nearPlane;
    float farPlane;
};
static_assert(sizeof(DofUniforms) == 32, "DofUniforms must be 32 bytes");

static const char* kDofShaderSource = R"WGSL(
struct DofUB {
    focalPlane: f32,
    blurDepth: f32,
    maxBlur: f32,
    minBlur: f32,
    texelSizeX: f32,
    texelSizeY: f32,
    nearPlane: f32,
    farPlane: f32,
};

@group(0) @binding(0) var sceneTex: texture_2d<f32>;
@group(0) @binding(1) var depthTex: texture_2d<f32>;
@group(0) @binding(2) var sceneSampler: sampler;
@group(0) @binding(3) var<uniform> dof: DofUB;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_dof(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

fn linearizeDepth(d: f32, near: f32, far: f32) -> f32 {
    return near * far / (far - d * (far - near));
}

const poissonDisc = array<vec2f, 8>(
    vec2f(-0.613392, 0.617481),
    vec2f( 0.170019,-0.040254),
    vec2f(-0.299417, 0.791925),
    vec2f( 0.645680, 0.493210),
    vec2f(-0.651784, 0.717887),
    vec2f( 0.421003, 0.027070),
    vec2f(-0.817194,-0.271096),
    vec2f( 0.977050,-0.108615),
);

@fragment fn fs_dof(in: VOut) -> @location(0) vec4f {
    let color = textureSample(sceneTex, sceneSampler, in.uv);
    let rawDepth = textureSample(depthTex, sceneSampler, in.uv).r;
    let linearDepth = linearizeDepth(rawDepth, dof.nearPlane, dof.farPlane);

    let diff = abs(linearDepth - dof.focalPlane);
    let coc = clamp(diff / max(dof.blurDepth, 0.001), dof.minBlur, dof.maxBlur);

    if (coc < 0.01) {
        return color;
    }

    let radius = coc;
    let texel = vec2f(dof.texelSizeX, dof.texelSizeY);
    var blurred = color.rgb;
    for (var i = 0; i < 8; i++) {
        let offset = poissonDisc[i] * radius * texel * 8.0;
        blurred += textureSample(sceneTex, sceneSampler, in.uv + offset).rgb;
    }
    blurred /= 9.0;

    return vec4f(mix(color.rgb, blurred, coc), color.a);
}
)WGSL";

static const char* kDepthResolveShaderSource = R"WGSL(
@group(0) @binding(0) var depthTexMS: texture_depth_multisampled_2d;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_depth_resolve(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

@fragment fn fs_depth_resolve(in: VOut) -> @location(0) vec4f {
    let coords = vec2i(in.pos.xy);
    let d = textureLoad(depthTexMS, coords, 0);
    return vec4f(d, 0.0, 0.0, 1.0);
}
)WGSL";

void DofPass::EnsurePipeline(GpuDevice& gpu) {
    if (mDofReady) return;
    auto& dev = gpu.Device();

    SamplerDesc sd{};
    mDefaultSampler = gpu.GetSampler(sd);

    // Depth resolve
    {
        wgpu::ShaderSourceWGSL src;
        src.code = kDepthResolveShaderSource;
        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.nextInChain = &src;
        auto shader = dev.CreateShaderModule(&smDesc);

        wgpu::BindGroupLayoutEntry entry{};
        entry.binding = 0;
        entry.visibility = wgpu::ShaderStage::Fragment;
        entry.texture.sampleType = wgpu::TextureSampleType::Depth;
        entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        entry.texture.multisampled = true;

        wgpu::BindGroupLayoutDescriptor bglDesc{};
        bglDesc.entryCount = 1;
        bglDesc.entries = &entry;
        mDepthResolveBGL = dev.CreateBindGroupLayout(&bglDesc);

        wgpu::PipelineLayoutDescriptor plDesc{};
        plDesc.bindGroupLayoutCount = 1;
        plDesc.bindGroupLayouts = &mDepthResolveBGL;
        mDepthResolvePipelineLayout = dev.CreatePipelineLayout(&plDesc);

        wgpu::ColorTargetState ct{};
        ct.format = wgpu::TextureFormat::R32Float;
        ct.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState frag{};
        frag.module = shader;
        frag.entryPoint = "fs_depth_resolve";
        frag.targetCount = 1;
        frag.targets = &ct;

        wgpu::RenderPipelineDescriptor pipeDesc{};
        pipeDesc.layout = mDepthResolvePipelineLayout;
        pipeDesc.vertex.module = shader;
        pipeDesc.vertex.entryPoint = "vs_depth_resolve";
        pipeDesc.fragment = &frag;
        pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mDepthResolvePipeline = dev.CreateRenderPipeline(&pipeDesc);
    }

    // DOF
    {
        wgpu::ShaderSourceWGSL src;
        src.code = kDofShaderSource;
        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.nextInChain = &src;
        mDofShader = dev.CreateShaderModule(&smDesc);

        wgpu::BindGroupLayoutEntry entries[4] = {};
        entries[0].binding = 0;
        entries[0].visibility = wgpu::ShaderStage::Fragment;
        entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
        entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        entries[1].binding = 1;
        entries[1].visibility = wgpu::ShaderStage::Fragment;
        entries[1].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
        entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        entries[2].binding = 2;
        entries[2].visibility = wgpu::ShaderStage::Fragment;
        entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
        entries[3].binding = 3;
        entries[3].visibility = wgpu::ShaderStage::Fragment;
        entries[3].buffer.type = wgpu::BufferBindingType::Uniform;
        entries[3].buffer.minBindingSize = sizeof(DofUniforms);

        wgpu::BindGroupLayoutDescriptor bglDesc{};
        bglDesc.entryCount = 4;
        bglDesc.entries = entries;
        mDofBGL = dev.CreateBindGroupLayout(&bglDesc);

        wgpu::PipelineLayoutDescriptor plDesc{};
        plDesc.bindGroupLayoutCount = 1;
        plDesc.bindGroupLayouts = &mDofBGL;
        mDofPipelineLayout = dev.CreatePipelineLayout(&plDesc);

        wgpu::BufferDescriptor bufDesc{};
        bufDesc.size = sizeof(DofUniforms);
        bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        mDofUniformBuffer = dev.CreateBuffer(&bufDesc);

        wgpu::ColorTargetState ct{};
        ct.format = gpu.SurfaceFormat();
        ct.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState frag{};
        frag.module = mDofShader;
        frag.entryPoint = "fs_dof";
        frag.targetCount = 1;
        frag.targets = &ct;

        wgpu::RenderPipelineDescriptor pipeDesc{};
        pipeDesc.layout = mDofPipelineLayout;
        pipeDesc.vertex.module = mDofShader;
        pipeDesc.vertex.entryPoint = "vs_dof";
        pipeDesc.fragment = &frag;
        pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mDofPipeline = dev.CreateRenderPipeline(&pipeDesc);
    }

    mDofReady = true;
}

void DofPass::Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
                  wgpu::Texture& intermediateTex, wgpu::TextureView& depthView,
                  int width, int height, GpuDevice& gpu) {
    extern DOFProc* TheDOFProc;
    if (!TheDOFProc || !TheDOFProc->Enabled()) return;

    EnsurePipeline(gpu);
    auto& dev = gpu.Device();
    auto& queue = gpu.Queue();

    // Ensure depth resolve texture
    if (mDofWidth != width || mDofHeight != height || !mDepthResolveTex) {
        wgpu::TextureDescriptor desc{};
        desc.size = {(uint32_t)width, (uint32_t)height, 1};
        desc.format = wgpu::TextureFormat::R32Float;
        desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        desc.mipLevelCount = 1;
        mDepthResolveTex = dev.CreateTexture(&desc);
        mDepthResolveView = mDepthResolveTex.CreateView();

        wgpu::TextureDescriptor iDesc{};
        iDesc.size = {(uint32_t)width, (uint32_t)height, 1};
        iDesc.format = gpu.SurfaceFormat();
        iDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        iDesc.mipLevelCount = 1;
        mDofIntermediateTex = dev.CreateTexture(&iDesc);
        mDofIntermediateView = mDofIntermediateTex.CreateView();
        mDofWidth = width;
        mDofHeight = height;
    }

    // Step 1: Resolve MSAA depth
    {
        wgpu::BindGroupEntry bgEntry{};
        bgEntry.binding = 0;
        bgEntry.textureView = depthView;

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = mDepthResolveBGL;
        bgDesc.entryCount = 1;
        bgDesc.entries = &bgEntry;
        wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mDepthResolveView;
        colorAtt.loadOp = wgpu::LoadOp::Clear;
        colorAtt.storeOp = wgpu::StoreOp::Store;
        colorAtt.clearValue = {1, 0, 0, 1};

        wgpu::RenderPassDescriptor rpDesc{};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;

        auto pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetPipeline(mDepthResolvePipeline);
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    }

    // Step 2: DOF blur
    {
        RndCam* cam = RndCam::Current();
        float nearPlane = cam ? cam->NearPlane() : 0.1f;
        float farPlane = cam ? cam->FarPlane() : 1000.0f;

        DofUniforms uni{};
        uni.focalPlane = TheDOFProc->FocalPlane();
        uni.blurDepth = TheDOFProc->BlurDepth();
        uni.maxBlur = TheDOFProc->MaxBlur();
        uni.minBlur = TheDOFProc->MinBlur();
        uni.texelSizeX = 1.0f / width;
        uni.texelSizeY = 1.0f / height;
        uni.nearPlane = nearPlane;
        uni.farPlane = farPlane;
        queue.WriteBuffer(mDofUniformBuffer, 0, &uni, sizeof(uni));

        wgpu::BindGroupEntry bgEntries[4] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].textureView = intermediateView;
        bgEntries[1].binding = 1;
        bgEntries[1].textureView = mDepthResolveView;
        bgEntries[2].binding = 2;
        bgEntries[2].sampler = mDefaultSampler;
        bgEntries[3].binding = 3;
        bgEntries[3].buffer = mDofUniformBuffer;
        bgEntries[3].size = sizeof(DofUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = mDofBGL;
        bgDesc.entryCount = 4;
        bgDesc.entries = bgEntries;
        wgpu::BindGroup bg = dev.CreateBindGroup(&bgDesc);

        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mDofIntermediateView;
        colorAtt.loadOp = wgpu::LoadOp::Clear;
        colorAtt.storeOp = wgpu::StoreOp::Store;
        colorAtt.clearValue = {0, 0, 0, 1};

        wgpu::RenderPassDescriptor rpDesc{};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;

        auto pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetPipeline(mDofPipeline);
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    }

    // Step 3: Swap results back to intermediate
    std::swap(intermediateView, mDofIntermediateView);
    std::swap(intermediateTex, mDofIntermediateTex);
}

void DofPass::Terminate() {
    mDofIntermediateTex = nullptr;
    mDofIntermediateView = nullptr;
    mDepthResolveTex = nullptr;
    mDepthResolveView = nullptr;
    mDofShader = nullptr;
    mDofBGL = nullptr;
    mDofPipelineLayout = nullptr;
    mDofPipeline = nullptr;
    mDepthResolvePipeline = nullptr;
    mDepthResolveBGL = nullptr;
    mDepthResolvePipelineLayout = nullptr;
    mDofUniformBuffer = nullptr;
    mDefaultSampler = nullptr;
    mDofReady = false;
}
