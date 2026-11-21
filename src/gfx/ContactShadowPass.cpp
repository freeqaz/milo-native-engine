// ContactShadowPass — see ContactShadowPass.h.
#include "gfx/ContactShadowPass.h"

#include "gfx/GpuDevice.h"

#include <cstring>

namespace {

const char* kContactShadowWgsl = R"(
struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_shadow(@location(0) clip: vec4f, @location(1) uv: vec2f) -> VOut {
    var o: VOut;
    o.pos = clip;
    o.uv = uv;
    return o;
}

const kStrength: f32 = 0.45;

fn shadow_factor(uv: vec2f) -> f32 {
    let r2 = dot(uv, uv);
    let e = exp(-3.0);
    let g = clamp((exp(-3.0 * r2) - e) / (1.0 - e), 0.0, 1.0);
    return 1.0 - kStrength * g;
}

// Unorm target: the blend multiplies the stored (display-referred) value.
@fragment fn fs_shadow(i: VOut) -> @location(0) vec4f {
    let f = shadow_factor(i.uv);
    return vec4f(f, f, f, 1.0);
}

// sRGB target: the blend runs in linear light.
@fragment fn fs_shadow_srgb(i: VOut) -> @location(0) vec4f {
    let f = pow(shadow_factor(i.uv), 2.2);
    return vec4f(f, f, f, 1.0);
}
)";

bool IsSrgb(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::RGBA8UnormSrgb || f == wgpu::TextureFormat::BGRA8UnormSrgb;
}

}  // namespace

void ContactShadowPass::Terminate() {
    mPipes.clear();
    mLayout = nullptr;
    mShader = nullptr;
    mVB = nullptr;
    mUsed = 0;
}

wgpu::RenderPipeline ContactShadowPass::PipelineFor(GpuDevice& gpu, wgpu::TextureFormat format,
                                                    uint32_t samples) {
    for (const Entry& e : mPipes)
        if (e.format == format && e.samples == samples) return e.pipe;

    wgpu::Device& dev = gpu.Device();
    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kContactShadowWgsl;
        wgpu::ShaderModuleDescriptor sm{};
        sm.nextInChain = &src;
        sm.label = "ContactShadow";
        mShader = dev.CreateShaderModule(&sm);
        // No bind groups: the scene's group 0 stays bound for the next mesh draw.
        wgpu::PipelineLayoutDescriptor pl{};
        pl.bindGroupLayoutCount = 0;
        mLayout = dev.CreatePipelineLayout(&pl);
    }

    // colour = dst * src; alpha untouched (and not written).
    wgpu::BlendState bs{};
    bs.color.operation = wgpu::BlendOperation::Add;
    bs.color.srcFactor = wgpu::BlendFactor::Dst;
    bs.color.dstFactor = wgpu::BlendFactor::Zero;
    bs.alpha.operation = wgpu::BlendOperation::Add;
    bs.alpha.srcFactor = wgpu::BlendFactor::Zero;
    bs.alpha.dstFactor = wgpu::BlendFactor::One;

    wgpu::ColorTargetState ct{};
    ct.format = format;
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;

    wgpu::FragmentState frag{};
    frag.module = mShader;
    frag.entryPoint = IsSrgb(format) ? "fs_shadow_srgb" : "fs_shadow";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[2] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x4;
    attrs[0].offset = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2;
    attrs[1].offset = 16;
    attrs[1].shaderLocation = 1;
    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(Vertex);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 2;
    vbl.attributes = attrs;

    // Tested against the world and the characters already drawn; writes none.
    wgpu::DepthStencilState ds{};
    ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
    ds.depthWriteEnabled = wgpu::OptionalBool::False;
    ds.depthCompare = wgpu::CompareFunction::LessEqual;

    wgpu::RenderPipelineDescriptor pd{};
    pd.label = "ContactShadow";
    pd.layout = mLayout;
    pd.vertex.module = mShader;
    pd.vertex.entryPoint = "vs_shadow";
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vbl;
    pd.fragment = &frag;
    pd.depthStencil = &ds;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.primitive.cullMode = wgpu::CullMode::None;
    pd.multisample.count = samples;
    wgpu::RenderPipeline pipe = dev.CreateRenderPipeline(&pd);
    mPipes.push_back({format, samples, pipe});
    return pipe;
}

void ContactShadowPass::Warm(GpuDevice& gpu, wgpu::TextureFormat format, uint32_t samples) {
    PipelineFor(gpu, format, samples);
}

int ContactShadowPass::Draw(GpuDevice& gpu, wgpu::RenderPassEncoder& pass, wgpu::TextureFormat format,
                            uint32_t samples, const std::vector<Vertex>& verts) {
    if (verts.empty()) return 0;
    const uint64_t capacity = (uint64_t)kMaxShadowsPerFrame * 6 * sizeof(Vertex);
    if (!mVB) {
        wgpu::BufferDescriptor bd{};
        bd.label = "ContactShadowVB";
        bd.size = capacity;
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        mVB = gpu.Device().CreateBuffer(&bd);
    }
    // Each flush of a frame writes its own range: Queue::WriteBuffer lands
    // before the frame's commands run, so a second flush may not overwrite the first.
    uint64_t bytes = (uint64_t)verts.size() * sizeof(Vertex);
    if (mUsed + bytes > capacity) bytes = (capacity - mUsed) / (6 * sizeof(Vertex)) * (6 * sizeof(Vertex));
    if (bytes == 0) return 0;
    gpu.Queue().WriteBuffer(mVB, mUsed, verts.data(), bytes);
    pass.SetPipeline(PipelineFor(gpu, format, samples));
    pass.SetVertexBuffer(0, mVB, mUsed, bytes);
    const uint32_t n = (uint32_t)(bytes / sizeof(Vertex));
    pass.Draw(n);
    mUsed += bytes;
    return (int)(n / 6);
}
