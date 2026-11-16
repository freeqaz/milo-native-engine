// TexBlendPass — see TexBlendPass.h.
#include "gfx/TexBlendPass.h"
#include "gfx/BindGroupCount.h"
#include "gfx/BindGroupCache.h"
#include "gfx/GpuTimers.h"
#include "gfx/GpuDevice.h"

#include <cstring>
#include "gfx/PipelineCount.h"

namespace {

// Base: a full-target triangle; the base map is sampled at the pixel centre's
// UV, which is the texel itself when the base map is the target's size.
// Unwrap: retail unwrapuv. The vertex's UV is its position; the pixel takes
// the texel's colour and the layer's alpha.
const char* kShader = R"WGSL(
struct Params {
    alpha: f32,
    invW: f32,
    invH: f32,
    pad: f32,
};
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var srcTex: texture_2d<f32>;
@group(0) @binding(2) var srcSampler: sampler;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_base(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let x = f32((i << 1u) & 2u);
    let y = f32(i & 2u);
    return vec4f(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
}

@fragment fn fs_base(@builtin(position) p: vec4f) -> @location(0) vec4f {
    let uv = vec2f(p.x * params.invW, p.y * params.invH);
    return textureSampleLevel(srcTex, srcSampler, uv, 0.0);
}

@vertex fn vs_unwrap(@location(0) uv: vec2f) -> VsOut {
    var o: VsOut;
    o.pos = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

@fragment fn fs_unwrap(in: VsOut) -> @location(0) vec4f {
    let t = textureSample(srcTex, srcSampler, in.uv);
    return vec4f(t.rgb, params.alpha);
}
)WGSL";

constexpr uint64_t kParamStride = 256;  // minUniformBufferOffsetAlignment
constexpr uint64_t kParamBytes = 16;

}  // namespace

TexBlendPass::Pipelines* TexBlendPass::EnsurePipelines(wgpu::TextureFormat format,
                                                       GpuDevice& gpu) {
    for (auto& p : mPipelines) {
        if (p.format == format) return &p;
    }
    wgpu::Device& dev = gpu.Device();
    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kShader;
        wgpu::ShaderModuleDescriptor sm{};
        sm.nextInChain = &src;
        sm.label = "TexBlend";
        mShader = dev.CreateShaderModule(&sm);

        wgpu::BindGroupLayoutEntry e[3] = {};
        e[0].binding = 0;
        e[0].visibility = wgpu::ShaderStage::Fragment;
        e[0].buffer.type = wgpu::BufferBindingType::Uniform;
        e[0].buffer.hasDynamicOffset = true;
        e[0].buffer.minBindingSize = kParamBytes;
        e[1].binding = 1;
        e[1].visibility = wgpu::ShaderStage::Fragment;
        e[1].texture.sampleType = wgpu::TextureSampleType::Float;
        e[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[2].binding = 2;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor bd{};
        bd.label = "TexBlend";
        bd.entryCount = 3;
        bd.entries = e;
        mBgl = dev.CreateBindGroupLayout(&bd);

        wgpu::PipelineLayoutDescriptor pl{};
        pl.label = "TexBlend";
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &mBgl;
        mLayout = dev.CreatePipelineLayout(&pl);

        // SetupMaterial: kTexWrapClamp; the work material filters linearly.
        wgpu::SamplerDescriptor sd{};
        sd.label = "TexBlend";
        sd.addressModeU = wgpu::AddressMode::ClampToEdge;
        sd.addressModeV = wgpu::AddressMode::ClampToEdge;
        sd.magFilter = wgpu::FilterMode::Linear;
        sd.minFilter = wgpu::FilterMode::Linear;
        sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        mSampler = dev.CreateSampler(&sd);
    }

    Pipelines p;
    p.format = format;

    wgpu::ColorTargetState ct{};
    ct.format = format;
    ct.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState fs{};
    fs.module = mShader;
    fs.targetCount = 1;
    fs.targets = &ct;

    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = mLayout;
    pd.vertex.module = mShader;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.primitive.cullMode = wgpu::CullMode::None;
    pd.multisample.count = 1;
    pd.fragment = &fs;

    // Base rect: kBlendSrc (blend off).
    pd.label = "TexBlendBase";
    pd.vertex.entryPoint = "vs_base";
    pd.vertex.bufferCount = 0;
    fs.entryPoint = "fs_base";
    ct.blend = nullptr;
    p.base = CreateCountedRenderPipeline(dev, &pd);

    // Unwrapped layers: kBlendSrcAlpha.
    wgpu::BlendState blend{};
    blend.color.operation = wgpu::BlendOperation::Add;
    blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha = blend.color;
    ct.blend = &blend;
    fs.entryPoint = "fs_unwrap";
    pd.vertex.entryPoint = "vs_unwrap";
    wgpu::VertexAttribute attr{};
    attr.format = wgpu::VertexFormat::Float32x2;
    attr.offset = kUVOffset;
    attr.shaderLocation = 0;
    wgpu::VertexBufferLayout vbl{};
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 1;
    vbl.attributes = &attr;
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vbl;
    pd.label = "TexBlendUnwrap64";
    vbl.arrayStride = 64;
    p.unwrap64 = CreateCountedRenderPipeline(dev, &pd);
    pd.label = "TexBlendUnwrap88";
    vbl.arrayStride = 88;
    p.unwrap88 = CreateCountedRenderPipeline(dev, &pd);

    if (!p.base || !p.unwrap64 || !p.unwrap88) return nullptr;
    mPipelines.push_back(p);
    return &mPipelines.back();
}

bool TexBlendPass::Record(wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
                          wgpu::TextureFormat format, uint32_t w, uint32_t h,
                          const wgpu::TextureView& base, const Layer* layers, size_t count,
                          GpuDevice& gpu, BindGroupCache* cache) {
    if (!encoder || !target || w == 0 || h == 0) return false;
    Pipelines* pipes = EnsurePipelines(format, gpu);
    if (!pipes) return false;
    wgpu::Device& dev = gpu.Device();

    // One parameter block per draw, each in its own ring slot.
    const size_t draws = count + 1;
    if (!mParams || mFrameUsed + draws > mParamSlots) {
        uint32_t slots = mParamSlots ? mParamSlots : 64;
        while (slots < mFrameUsed + draws) slots *= 2;
        wgpu::BufferDescriptor bd{};
        bd.label = "TexBlendParams";
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        bd.size = (uint64_t)slots * kParamStride;
        mParams = dev.CreateBuffer(&bd);
        mParamSlots = slots;
        mParamNext = 0;
        mFrameUsed = 0;  // the old buffer keeps this frame's earlier slots
    }
    if (mParamNext + draws > mParamSlots) mParamNext = 0;
    const uint32_t first = mParamNext;
    mParamNext += (uint32_t)draws;
    mFrameUsed += (uint32_t)draws;
    std::vector<uint8_t> params(draws * kParamStride, 0);
    auto put = [&](size_t i, float alpha) {
        float v[4] = {alpha, 1.0f / (float)w, 1.0f / (float)h, 0.0f};
        memcpy(&params[i * kParamStride], v, sizeof(v));
    };
    put(0, 1.0f);
    for (size_t i = 0; i < count; i++) put(i + 1, layers[i].alpha);
    gpu.Queue().WriteBuffer(mParams, (uint64_t)first * kParamStride, params.data(), params.size());
    auto offsetOf = [&](size_t i) { return (uint32_t)((first + i) * kParamStride); };

    // The group binds the ring at offset 0 (the draw's slot is the dynamic
    // offset), the texture and the sampler: one group per texture.
    auto bindGroup = [&](const wgpu::TextureView& tex) {
        wgpu::BindGroupEntry e[3] = {};
        e[0].binding = 0;
        e[0].buffer = mParams;
        e[0].offset = 0;
        e[0].size = kParamBytes;
        e[1].binding = 1;
        e[1].textureView = tex;
        e[2].binding = 2;
        e[2].sampler = mSampler;
        wgpu::BindGroupDescriptor d{};
        d.label = "TexBlend";
        d.layout = mBgl;
        d.entryCount = 3;
        d.entries = e;
        return cache ? cache->GetOrCreate(dev, d) : CreateCountedBindGroup(dev, &d);
    };

    wgpu::RenderPassColorAttachment ca{};
    ca.view = target;
    ca.loadOp = wgpu::LoadOp::Load;
    ca.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp{};
    rp.label = "TexBlendPass";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    MILO_GPU_TIME_PASS(rp);
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    pass.SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);

    if (base) {
        pass.SetPipeline(pipes->base);
        const uint32_t off = offsetOf(0);
        pass.SetBindGroup(0, bindGroup(base), 1, &off);
        pass.Draw(3, 1, 0, 0);
    }
    for (size_t i = 0; i < count; i++) {
        const Layer& l = layers[i];
        if (!l.vertexBuffer || !l.indexBuffer || !l.tex || l.indexCount == 0) continue;
        if (l.vertexStride != 64 && l.vertexStride != 88) continue;
        pass.SetPipeline(l.vertexStride == 64 ? pipes->unwrap64 : pipes->unwrap88);
        const uint32_t off = offsetOf(i + 1);
        pass.SetBindGroup(0, bindGroup(l.tex), 1, &off);
        pass.SetVertexBuffer(0, l.vertexBuffer, 0, l.vertexBytes);
        pass.SetIndexBuffer(l.indexBuffer, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
        pass.DrawIndexed(l.indexCount, 1, 0, 0, 0);
    }
    pass.End();
    return true;
}

void TexBlendPass::Terminate() {
    mParams = nullptr;
    mParamSlots = 0;
    mParamNext = 0;
    mFrameUsed = 0;
    mPipelines.clear();
    mSampler = nullptr;
    mLayout = nullptr;
    mBgl = nullptr;
    mShader = nullptr;
}
