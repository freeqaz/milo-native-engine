// PresentScale -- see PresentScale.h.
#include "gfx/PresentScale.h"
#include "gfx/BindGroupCount.h"
#include "gfx/GpuTimers.h"
#include "gfx/PipelineCount.h"

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

// The same one tap with the frame's alpha kept (SetKeepAlpha): a copy that is
// not the present. DC3's bloom source is one, and its alpha is the bloom mask.
@fragment fn fs_copy(in: VsOut) -> @location(0) vec4f {
    return textureSampleLevel(frameTex, frameSampler, in.uv, 0.0);
}

// Downscale (render scale): four bilinear taps at +-1/4 of the output pixel's
// footprint in frame texels. At exactly 2x each tap lands on a texel centre,
// so the four are the 2x2 box; at 1.5x they cover the 1.5-texel footprint.
@fragment fn fs_box(in: VsOut) -> @location(0) vec4f {
    let o = vec2f(abs(dpdx(in.uv.x)), abs(dpdy(in.uv.y))) * 0.25;  // uv per output pixel / 4
    let c = textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(-o.x, -o.y), 0.0) +
            textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(o.x, -o.y), 0.0) +
            textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(-o.x, o.y), 0.0) +
            textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(o.x, o.y), 0.0);
    // Alpha averaged too: a readback keeps the frame's alpha (the bloom mask)
    // as an unscaled frame's does; the window ignores it.
    return c * 0.25;
}

// fs_box with a fixed ordered dither (PresentScale::SetDither): the box mean,
// a fraction of an 8-bit level apart from its neighbours, is written as
// floor(v * 255 + t) / 255 with t a 4x4 Bayer threshold in (0, 1) -- (i + 0.5)
// / 16, never 0 or 1 -- so a mean that IS an 8-bit level (four equal texels:
// a flat fill, the inside of a glyph) comes out exactly that level, and one
// between two levels comes out as the two in the proportion of its fraction
// over each 4x4 block of output pixels. The pattern is keyed on the OUTPUT
// pixel (uv over the output pixel's size), not the target position, so a
// letterboxed present and an origin-0 readback dither alike. Static: it does
// not move from frame to frame.
@fragment fn fs_box_dither(in: VsOut) -> @location(0) vec4f {
    var bayer = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                               3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    let o = vec2f(abs(dpdx(in.uv.x)), abs(dpdy(in.uv.y))) * 0.25;
    let c = (textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(-o.x, -o.y), 0.0) +
             textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(o.x, -o.y), 0.0) +
             textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(-o.x, o.y), 0.0) +
             textureSampleLevel(frameTex, frameSampler, in.uv + vec2f(o.x, o.y), 0.0)) * 0.25;
    let px = vec2u(floor(in.uv / (4.0 * o)));
    let t = (bayer[(px.y & 3u) * 4u + (px.x & 3u)] + 0.5) / 16.0;
    return vec4f(floor(c.rgb * 255.0 + t) / 255.0, c.a);
}
)WGSL";

// The dither writes 8-bit levels of the value it is given, so only into a
// linear 8-bit target (an sRGB target quantises after its encode).
bool DitherableFormat(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::RGBA8Unorm || f == wgpu::TextureFormat::BGRA8Unorm;
}

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
        mPipe = CreateCountedRenderPipeline(device, &pd);
        frag.entryPoint = "fs_box";
        pd.label = "PresentScaleBox";
        mBoxPipe = CreateCountedRenderPipeline(device, &pd);
        mBoxDitherPipe = nullptr;
        mCopyPipe = nullptr;
        mFormat = targetFormat;
    }
    const bool box = mBoxFilter && rect.w < fw && rect.h < fh;
    if (mKeepAlpha && !box && !mCopyPipe) {
        // Only once an alpha-keeping one-tap copy is asked for.
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &mBGL;
        wgpu::PipelineLayout pl = device.CreatePipelineLayout(&pld);
        wgpu::ColorTargetState ct{};
        ct.format = targetFormat;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = "fs_copy";
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "PresentScaleCopy";
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mCopyPipe = CreateCountedRenderPipeline(device, &pd);
    }
    const bool dither = box && mDither && DitherableFormat(targetFormat);
    if (dither && !mBoxDitherPipe) {
        // Only once a dithered box is asked for: with the dither off, none.
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &mBGL;
        wgpu::PipelineLayout pl = device.CreatePipelineLayout(&pld);
        wgpu::ColorTargetState ct{};
        ct.format = targetFormat;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mShader;
        frag.entryPoint = "fs_box_dither";
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "PresentScaleBoxDither";
        pd.layout = pl;
        pd.vertex.module = mShader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mBoxDitherPipe = CreateCountedRenderPipeline(device, &pd);
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
        mBindGroup = CreateCountedBindGroup(device, &bgd);
        mBoundFrame = frame;
        mBoundNearest = nearest;
    }

    wgpu::RenderPassColorAttachment att{};
    att.view = target;
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = mLabel;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    MILO_GPU_TIME_PASS(rp);
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
    if (frame && rect.w > 0 && rect.h > 0) {
        pass.SetViewport((float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h, 0.0f, 1.0f);
        pass.SetPipeline(dither ? mBoxDitherPipe : box ? mBoxPipe : mKeepAlpha ? mCopyPipe : mPipe);
        pass.SetBindGroup(0, mBindGroup);
        pass.Draw(3);
    }
    pass.End();
    return rect;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
const char* PresentScaleWgslSource() { return kShader; }
