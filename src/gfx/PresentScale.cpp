// PresentScale -- see PresentScale.h.
#include "gfx/PresentScale.h"

namespace {

// A triangle over the viewport; uv (0,0) at the frame's top-left.
const char* kShader = R"WGSL(
@group(0) @binding(0) var frameTex: texture_2d<f32>;
@group(0) @binding(1) var frameSampler: sampler;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> VsOut {
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    var out: VsOut;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

// Opaque: the window is not composited through the frame's alpha (the frame's
// alpha channel carries the shape's bloom mask, not coverage).
@fragment fn fs_present(in: VsOut) -> @location(0) vec4f {
    return vec4f(textureSampleLevel(frameTex, frameSampler, in.uv, 0.0).rgb, 1.0);
}
)WGSL";

}  // namespace

PresentRect FitPresentRect(int frameW, int frameH, int surfaceW, int surfaceH) {
    PresentRect r;
    if (frameW <= 0 || frameH <= 0 || surfaceW <= 0 || surfaceH <= 0) return r;
    // Wider than the frame: full height, pillarbox. Otherwise full width,
    // letterbox. 64-bit products: a 16k surface times a 16k frame overflows int.
    if ((long long)surfaceW * frameH > (long long)surfaceH * frameW) {
        r.h = surfaceH;
        r.w = (int)(((long long)surfaceH * frameW + frameH / 2) / frameH);
    } else {
        r.w = surfaceW;
        r.h = (int)(((long long)surfaceW * frameH + frameW / 2) / frameW);
    }
    if (r.w > surfaceW) r.w = surfaceW;
    if (r.h > surfaceH) r.h = surfaceH;
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = (surfaceW - r.w) / 2;
    r.y = (surfaceH - r.h) / 2;
    return r;
}

bool PresentRectIsIntegerScale(const PresentRect& r, int frameW, int frameH) {
    if (frameW <= 0 || frameH <= 0 || r.w <= 0 || r.h <= 0) return false;
    return r.w % frameW == 0 && r.h % frameH == 0 && r.w / frameW == r.h / frameH;
}

PresentRect PresentScale::Encode(wgpu::Device& device, wgpu::CommandEncoder& encoder,
                                 const wgpu::Texture& frame, const wgpu::TextureView& target,
                                 wgpu::TextureFormat targetFormat, int targetW, int targetH) {
    const int fw = frame ? (int)frame.GetWidth() : 0, fh = frame ? (int)frame.GetHeight() : 0;
    const PresentRect rect = FitPresentRect(fw, fh, targetW, targetH);

    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kShader;
        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.nextInChain = &src;
        smDesc.label = "PresentScale";
        mShader = device.CreateShaderModule(&smDesc);

        wgpu::BindGroupLayoutEntry e[2]{};
        e[0].binding = 0;
        e[0].visibility = wgpu::ShaderStage::Fragment;
        e[0].texture.sampleType = wgpu::TextureSampleType::Float;
        e[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[1].binding = 1;
        e[1].visibility = wgpu::ShaderStage::Fragment;
        e[1].sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor bd{};
        bd.entryCount = 2;
        bd.entries = e;
        mBGL = device.CreateBindGroupLayout(&bd);

        wgpu::SamplerDescriptor sd{};
        sd.addressModeU = wgpu::AddressMode::ClampToEdge;
        sd.addressModeV = wgpu::AddressMode::ClampToEdge;
        sd.minFilter = wgpu::FilterMode::Linear;
        sd.magFilter = wgpu::FilterMode::Linear;
        sd.label = "PresentScaleLinear";
        mLinear = device.CreateSampler(&sd);
        sd.minFilter = wgpu::FilterMode::Nearest;
        sd.magFilter = wgpu::FilterMode::Nearest;
        sd.label = "PresentScaleNearest";
        mNearest = device.CreateSampler(&sd);
    }
    if (!mPipe || mFormat != targetFormat) {
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &mBGL;
        wgpu::PipelineLayout pl = device.CreatePipelineLayout(&pld);
        wgpu::ColorTargetState ct{};
        ct.format = targetFormat;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = "fs_present";
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "PresentScale";
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mPipe = device.CreateRenderPipeline(&pd);
        mFormat = targetFormat;
    }
    const bool nearest = PresentRectIsIntegerScale(rect, fw, fh);
    if (frame && (frame.Get() != mBoundFrame.Get() || nearest != mBoundNearest || !mBindGroup)) {
        wgpu::BindGroupEntry be[2]{};
        be[0].binding = 0;
        be[0].textureView = frame.CreateView();
        be[1].binding = 1;
        be[1].sampler = nearest ? mNearest : mLinear;
        wgpu::BindGroupDescriptor bgd{};
        bgd.label = "PresentScale";
        bgd.layout = mBGL;
        bgd.entryCount = 2;
        bgd.entries = be;
        mBindGroup = device.CreateBindGroup(&bgd);
        mBoundFrame = frame;
        mBoundNearest = nearest;
    }

    wgpu::RenderPassColorAttachment att{};
    att.view = target;
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "PresentScalePass";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    if (frame && rect.w > 0 && rect.h > 0) {
        pass.SetViewport((float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h, 0.0f, 1.0f);
        pass.SetPipeline(mPipe);
        pass.SetBindGroup(0, mBindGroup);
        pass.Draw(3);
    }
    pass.End();
    return rect;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
const char* PresentScaleWgslSource() { return kShader; }
