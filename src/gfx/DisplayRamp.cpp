// DisplayRamp — see DisplayRamp.h.
#include "gfx/DisplayRamp.h"
#include "gfx/GpuDevice.h"

#include <cstdio>

namespace {

const char* kShader = R"WGSL(
override kGamma: f32 = 1.0;

@group(0) @binding(0) var srcTex: texture_2d<f32>;

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4f {
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    return vec4f(x, y, 0.0, 1.0);
}

// DxRnd::SetupGamma's table, entry i = (u16)(pow(i / 256, g) * 1024) * 64.
@fragment fn fs_ramp(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let c = textureLoad(srcTex, vec2i(pos.xy), 0);
    let i = round(clamp(c.rgb, vec3f(0.0), vec3f(1.0)) * 255.0);
    let p = select(vec3f(0.0), pow(max(i, vec3f(1.0)) / 256.0, vec3f(kGamma)), i > vec3f(0.0));
    let entry = floor(p * 1024.0) * 64.0;
    return vec4f(min(entry, vec3f(65535.0)) / 65535.0, c.a);
}
)WGSL";

}  // namespace

bool DisplayRamp::Apply(wgpu::CommandEncoder& encoder, const wgpu::Texture& frameTex,
                        const wgpu::TextureView& frameView, float gamma, GpuDevice& gpu) {
    if (!frameTex || !frameView || gamma <= 0.0f) return false;
    if (!(frameTex.GetUsage() & wgpu::TextureUsage::CopySrc)) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "DisplayRamp: the frame texture does not allow CopySrc; "
                            "presenting without the display gamma ramp\n");
            warned = true;
        }
        return false;
    }
    auto& dev = gpu.Device();
    const wgpu::TextureFormat fmt = frameTex.GetFormat();
    const uint32_t w = frameTex.GetWidth(), h = frameTex.GetHeight();

    if (!mPipe || mFormat != fmt || mGamma != gamma) {
        if (!mShader) {
            wgpu::ShaderSourceWGSL src;
            src.code = kShader;
            wgpu::ShaderModuleDescriptor smDesc{};
            smDesc.nextInChain = &src;
            smDesc.label = "DisplayRamp";
            mShader = dev.CreateShaderModule(&smDesc);

            wgpu::BindGroupLayoutEntry e{};
            e.binding = 0;
            e.visibility = wgpu::ShaderStage::Fragment;
            e.texture.sampleType = wgpu::TextureSampleType::Float;
            e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            wgpu::BindGroupLayoutDescriptor bd{};
            bd.entryCount = 1;
            bd.entries = &e;
            mBGL = dev.CreateBindGroupLayout(&bd);
        }
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &mBGL;
        wgpu::PipelineLayout pl = dev.CreatePipelineLayout(&pld);

        wgpu::ConstantEntry k{};
        k.key = "kGamma";
        k.value = gamma;
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = "fs_ramp";
        frag.constantCount = 1;
        frag.constants = &k;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "DisplayRamp";
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mPipe = dev.CreateRenderPipeline(&pd);
        mFormat = fmt;
        mGamma = gamma;
        mScratch = nullptr;
    }
    if (!mScratch || mW != w || mH != h) {
        wgpu::TextureDescriptor td{};
        td.label = "DisplayRampSource";
        td.size = {w, h, 1};
        td.format = fmt;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        mScratch = dev.CreateTexture(&td);
        mScratchView = mScratch.CreateView();
        mW = w;
        mH = h;
    }

    wgpu::TexelCopyTextureInfo src{};
    src.texture = frameTex;
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = mScratch;
    wgpu::Extent3D size = {w, h, 1};
    encoder.CopyTextureToTexture(&src, &dst, &size);

    wgpu::BindGroupEntry be{};
    be.binding = 0;
    be.textureView = mScratchView;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mBGL;
    bgd.entryCount = 1;
    bgd.entries = &be;
    wgpu::BindGroup bg = dev.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment att{};
    att.view = frameView;
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DisplayRampPass";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mPipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
    return true;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* DisplayRampWgslSource() { return kShader; }
