#include "gfx/DofPass.h"
#include "gfx/GpuTimers.h"
#include "gfx/GpuDevice.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Cam.h"
#include "rndobj/PostProc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "gfx/PipelineCount.h"

namespace {

struct BlurUniforms {
    float taps[8][4];  // xy = uv offset (SetVHBlurWeights: c31..c38)
};
static_assert(sizeof(BlurUniforms) == 128, "BlurUniforms layout");

struct DofUniforms {
    float c24[4];  // range, -scale * range, minBlur, maxBlur
};
static_assert(sizeof(DofUniforms) == 16, "DofUniforms layout");

// The two tap tables SetVHBlurWeights builds on first use (function-local
// statics in retail). Both are 2D; "horizontal" and "vertical" name the passes,
// not the directions.
const float kHorzTaps[8][2] = {
    {0.4432332516f, -0.9751155376f}, {0.5374298096f, -0.4737342f},
    {-0.2649691105f, -0.4189302325f}, {0.7919751406f, 0.1909018755f},
    {-0.2418884039f, 0.9970650673f}, {-0.8140995502f, 0.9143759012f},
    {0.1998412609f, 0.7864136696f}, {0.1438316107f, -0.1410079002f},
};
const float kVertTaps[8][2] = {
    {-0.9420162439f, -0.3990621567f}, {0.9455860853f, -0.768907249f},
    {-0.09418410063f, -0.9293887019f}, {0.3449593782f, 0.2938776016f},
    {-0.9158858061f, 0.4577143192f}, {-0.8154423237f, -0.8791246414f},
    {-0.3827754259f, 0.276768446f}, {0.9748439789f, 0.7564837933f},
};

const char* kDofShaderSource = R"WGSL(
struct BlurUB {
    taps: array<vec4f, 8>,
};
struct DofUB {
    c24: vec4f,   // range, -scale * range, minBlur, maxBlur
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> blur: BlurUB;

@group(0) @binding(3) var sceneTex: texture_2d<f32>;
@group(0) @binding(4) var blurTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_multisampled_2d;
@group(0) @binding(6) var<uniform> dof: DofUB;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> VOut {
    var out: VOut;
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    out.pos = vec4f(x, y, 0.0, 1.0);
    out.uv = vec2f((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    return out;
}

// downsample_4x.ps: the 4x4 box under each target texel (retail takes four
// bilinear taps over the same block).
@fragment fn fs_down4(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            acc += textureLoad(srcTex, min(base + vec2i(i, j), dims - 1), 0);
        }
    }
    return acc / 16.0;
}

// blur.ps, 8-tap permutation: sum of c47..c54 (all 0.125) times the source at
// uv + c31..c38. No centre tap.
@fragment fn fs_blur(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 8; i++) {
        acc += textureSampleLevel(srcTex, samp, in.uv + blur.taps[i].xy, 0.0);
    }
    return acc * 0.125;
}

// np-gfx-post round 10, the enhanced look (Run's `enhanced`): the downsample
// as a tent over the 8x8 texels around each 4x4 block, (1 - |d| / 4) per axis.
const kTent = array<f32, 8>(0.03125, 0.09375, 0.15625, 0.21875,
                            0.21875, 0.15625, 0.09375, 0.03125);
const kTentO = array<f32, 4>(-2.75, -0.9166667, 0.9166667, 2.75);
const kTentW = array<f32, 4>(0.125, 0.375, 0.375, 0.125);
@fragment fn fs_down_tent(in: VOut) -> @location(0) vec4f {
    // The same tent in 4 x 4 bilinear taps: per axis, texel pairs (-3.5,
    // -2.5) and (-1.5, -0.5) from the block centre weigh 1/8 and 3/8 and
    // their bilinear tap sits at -2.75 and -11/12 (and mirrored). Clamping
    // the sampler at the edge clamps each texel as textureLoad did.
    let size = vec2f(textureDimensions(srcTex));
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            acc += (kTentW[i] * kTentW[j]) *
                   textureSampleLevel(srcTex, samp, (c + vec2f(kTentO[i], kTentO[j])) / size, 0.0);
        }
    }
    return acc;
}

// np-look23 (c), the enhanced look's LookSettings::postDofDepthWeight: the
// same 8x8 tent, but each scene texel weighted by its own blur factor
// squared (DepthWeight), premultiplied, the weight sum in alpha. An in-focus
// texel (factor ~0) then adds nothing to the blur, so the blurred background
// next to an in-focus figure is background only: retail's depth-blind
// downsample spreads the figure's colour into it, a bright fringe ~12 px
// wide at 720 (Xenia and the hardware show it too). The blur passes average
// the weighted colour and the weight alike; the composite divides
// (fs_composite_bspline_dw). The quarter targets are RGBA16Float here, so the
// division does not amplify 8-bit rounding.
fn dof_factor(z: f32) -> f32 {
    let t = z * dof.c24.x + dof.c24.y;
    return clamp(min(max(abs(t), dof.c24.z), dof.c24.w), 0.0, 1.0);
}
// fs_down_tent's 16 bilinear taps, each weighted by the factor at the
// heavier texel of its pair (one depth load per tap): a quarter of the loads
// of weighting all 64 texels (0.187 -> 0.083 ms at 1080 on a 3090), the weight
// resolved to 2 scene texels at an edge.
@fragment fn fs_down_tent_dw(in: VOut) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(srcTex));
    let ddims = vec2i(textureDimensions(depthTex));
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let q = c + vec2f(kTentO[i], kTentO[j]);
            let f = dof_factor(textureLoad(depthTex, clamp(vec2i(floor(q)), vec2i(0), ddims - 1), 0));
            let w = kTentW[i] * kTentW[j] * f * f;
            acc += vec4f(textureSampleLevel(srcTex, samp, q / size, 0.0).rgb * w, w);
        }
    }
    return acc;
}
// np-look23 (c2), inspection variants of fs_down_tent_dw for timing
// (DofPass::kDepthWeightLoads*). The same output: premultiplied colour, the
// weight in alpha.
// 4: one depth load per 2x2 group of the taps, at the scene texel 1.5 from the
// block centre on each axis (the group's centre of weight, -1.375, rounded to
// a texel centre), its factor squared shared by the group's four taps.
@fragment fn fs_down_tent_dw4(in: VOut) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(srcTex));
    let dmax = vec2i(textureDimensions(depthTex)) - 1;
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    for (var gj = 0; gj < 2; gj++) {
        for (var gi = 0; gi < 2; gi++) {
            let g = c + vec2f(f32(gi) * 3.0 - 1.5, f32(gj) * 3.0 - 1.5);
            let f = dof_factor(textureLoad(depthTex, clamp(vec2i(floor(g)), vec2i(0), dmax), 0));
            var col = vec3f(0.0);
            var wsum = 0.0;
            for (var j = gj * 2; j < gj * 2 + 2; j++) {
                for (var i = gi * 2; i < gi * 2 + 2; i++) {
                    let w = kTentW[i] * kTentW[j];
                    col += w * textureSampleLevel(srcTex, samp, (c + vec2f(kTentO[i], kTentO[j])) / size, 0.0).rgb;
                    wsum += w;
                }
            }
            let ff = f * f;
            acc += vec4f(col * ff, wsum * ff);
        }
    }
    return acc;
}
// 1: one depth load at the block's centre (scene texel c, +0.5 from it); the
// plain tent times that one factor squared.
@fragment fn fs_down_tent_dw1(in: VOut) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(srcTex));
    let dmax = vec2i(textureDimensions(depthTex)) - 1;
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    let f = dof_factor(textureLoad(depthTex, clamp(vec2i(c), vec2i(0), dmax), 0));
    var col = vec3f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            col += (kTentW[i] * kTentW[j]) *
                   textureSampleLevel(srcTex, samp, (c + vec2f(kTentO[i], kTentO[j])) / size, 0.0).rgb;
        }
    }
    let ff = f * f;
    return vec4f(col * ff, ff);
}
// 0 (the half factor): a pre-pass at half the scene size writes the blur
// factor squared from one depth load per half-size texel (scene texel 2p + 1,
// so each multisampled depth texel is read at most once and the reads are
// coherent), and the downsample samples it bilinearly at each of its 16 taps
// -- a single-sample R16Float texture a quarter the scene's size, through the
// filtering sampler, in place of 16 multisampled depth loads.
@fragment fn fs_dw_factor_half(in: VOut) -> @location(0) vec4f {
    let dmax = vec2i(textureDimensions(depthTex)) - 1;
    let p = vec2i(in.pos.xy) * 2 + 1;
    let f = dof_factor(textureLoad(depthTex, min(p, dmax), 0));
    return vec4f(f * f, 0.0, 0.0, 1.0);
}
@group(0) @binding(7) var factorTex: texture_2d<f32>;
@fragment fn fs_down_tent_dwh(in: VOut) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(srcTex));
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let uv = (c + vec2f(kTentO[i], kTentO[j])) / size;
            let w = kTentW[i] * kTentW[j] * textureSampleLevel(factorTex, samp, uv, 0.0).r;
            acc += vec4f(textureSampleLevel(srcTex, samp, uv, 0.0).rgb * w, w);
        }
    }
    return acc;
}
// The weighted blur back to colour: rgb / weight, faded to the sharp scene
// where the weight is too small to carry a colour (a pixel ringed by in-focus
// texels; its own factor is small there, so the mix barely reads it).
@fragment fn fs_composite_bspline_dw(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let b = blur_bspline(in.uv);
    let rgb = mix(scene.rgb, b.rgb / max(b.a, 1e-4), clamp(b.a * 16.0, 0.0, 1.0));
    let z = textureLoad(depthTex, min(p, vec2i(textureDimensions(depthTex)) - 1), 0);
    let f = dof_factor(z);
    return vec4f(clamp(mix(scene.rgb, rgb, f), vec3f(0.0), vec3f(1.0)), scene.a);
}

// The quarter-size blur at uv through a cubic B-spline in four bilinear taps
// (Sigg and Hadwiger 2005): C2-smooth across its texel boundaries, where the
// bilinear read creases. A partition of unity.
fn blur_bspline(uv: vec2f) -> vec4f {
    let size = vec2f(textureDimensions(blurTex));
    let p = uv * size - 0.5;
    let i = floor(p);
    let f = p - i;
    let f2 = f * f;
    let f3 = f2 * f;
    let w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) * (1.0 / 6.0);
    let w1 = (4.0 - 6.0 * f2 + 3.0 * f3) * (1.0 / 6.0);
    let w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) * (1.0 / 6.0);
    let w3 = f3 * (1.0 / 6.0);
    let g0 = w0 + w1;
    let g1 = w2 + w3;
    let p0 = (i - 0.5 + w1 / g0) / size;
    let p1 = (i + 1.5 + w3 / g1) / size;
    return g0.x * g0.y * textureSampleLevel(blurTex, samp, p0, 0.0) +
           g1.x * g0.y * textureSampleLevel(blurTex, samp, vec2f(p1.x, p0.y), 0.0) +
           g0.x * g1.y * textureSampleLevel(blurTex, samp, vec2f(p0.x, p1.y), 0.0) +
           g1.x * g1.y * textureSampleLevel(blurTex, samp, p1, 0.0);
}

fn dof_mix(p: vec2i, blurred: vec4f) -> vec4f {
    let scene = textureLoad(sceneTex, p, 0);
    let z = textureLoad(depthTex, min(p, vec2i(textureDimensions(depthTex)) - 1), 0);
    let t = z * dof.c24.x + dof.c24.y;
    let f = clamp(min(max(abs(t), dof.c24.z), dof.c24.w), 0.0, 1.0);
    return clamp(mix(scene, blurred, f), vec4f(0.0), vec4f(1.0));
}
@fragment fn fs_composite_bspline(in: VOut) -> @location(0) vec4f {
    return dof_mix(vec2i(in.pos.xy), blur_bspline(in.uv));
}

// postprocess.ps, depth-of-field bit: tf6 scene, tf8 blur (linear, clamp),
// tf9 depth. Retail reads 1 - z because it runs reverse-Z.
@fragment fn fs_composite(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let blurred = textureSampleLevel(blurTex, samp, in.uv, 0.0);
    let z = textureLoad(depthTex, min(p, vec2i(textureDimensions(depthTex)) - 1), 0);
    let t = z * dof.c24.x + dof.c24.y;
    let f = clamp(min(max(abs(t), dof.c24.z), dof.c24.w), 0.0, 1.0);
    return clamp(mix(scene, blurred, f), vec4f(0.0), vec4f(1.0));
}
)WGSL";

// Projected depth of view distance d, as DxCam::ProjectZ before its reverse-Z
// flip; 0 when d is nearer than the near plane (NgDOFProc::Set's guard).
float ProjectedDepth(float d, float nearPlane, float farPlane, float zNear, float zFar) {
    if (d < nearPlane) return 0.0f;
    return (farPlane - farPlane / d * nearPlane) / (farPlane - nearPlane) * (zFar - zNear) + zNear;
}

}  // namespace

void DofPass::RetailConstants(float focalPlane, float blurDepth, float maxBlur, float minBlur,
                              float nearPlane, float farPlane, float zNear, float zFar,
                              float out[4]) {
    float scale = ProjectedDepth(focalPlane, nearPlane, farPlane, zNear, zFar);
    float bias = ProjectedDepth(focalPlane - focalPlane * blurDepth, nearPlane, farPlane,
                                zNear, zFar);
    if (scale < bias + 0.001f) scale = bias + 0.001f;
    float range = 1.0f / (scale - bias);
    out[0] = range;
    out[1] = -(scale * range);
    out[2] = std::min(maxBlur, minBlur);
    out[3] = maxBlur < 0.0f ? 1.0f : maxBlur;
}

void DofPass::RetailBlurOffsets(bool vertical, int blurW, int blurH, float blurWidthScale,
                                float out[8][2]) {
    const float (*taps)[2] = vertical ? kVertTaps : kHorzTaps;
    const float f = blurWidthScale * 0.666f;
    const float xScale = (float)blurW * f * 4.8828124e-06f;
    const float yScale = (float)blurH * f * 1.5432099e-05f;
    for (int i = 0; i < 8; i++) {
        out[i][0] = taps[i][0] * xScale * 5.0f;
        out[i][1] = taps[i][1] * yScale * 5.0f;
    }
}

// Retail's uv offsets grow with the blur target's size (the quarter of its
// 1280x720 frame). A render-scaled frame (GpuDeviceDesc::renderScale) takes
// them from its output size's quarter; at 1 this is RetailBlurOffsets(qw, qh).
void DofPass::SceneBlurOffsets(bool vertical, int sceneW, int sceneH, float renderScale,
                               float blurWidthScale, float out[8][2]) {
    const float rs = renderScale > 0.0f ? renderScale : 1.0f;
    const int qw = std::max(1, sceneW >> 2), qh = std::max(1, sceneH >> 2);
    const int offW = std::max(1, (int)((float)qw / rs + 0.5f));
    const int offH = std::max(1, (int)((float)qh / rs + 0.5f));
    RetailBlurOffsets(vertical, offW, offH, blurWidthScale, out);
}

void DofPass::EnsurePipelines(wgpu::TextureFormat format, GpuDevice& gpu) {
    if (mShader && mFormat == format) return;
    auto& dev = gpu.Device();
    mFormat = format;

    if (!mShader) {
        wgpu::ShaderSourceWGSL src;
        src.code = kDofShaderSource;
        wgpu::ShaderModuleDescriptor smDesc{};
        smDesc.nextInChain = &src;
        smDesc.label = "DofPass";
        mShader = dev.CreateShaderModule(&smDesc);

        SamplerDesc sd{};
        sd.addressU = wgpu::AddressMode::ClampToEdge;
        sd.addressV = wgpu::AddressMode::ClampToEdge;
        mSampler = gpu.GetSampler(sd);

        auto tex = [](uint32_t b) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = b;
            e.visibility = wgpu::ShaderStage::Fragment;
            e.texture.sampleType = wgpu::TextureSampleType::Float;
            e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
            return e;
        };
        auto ub = [](uint32_t b, uint64_t size) {
            wgpu::BindGroupLayoutEntry e{};
            e.binding = b;
            e.visibility = wgpu::ShaderStage::Fragment;
            e.buffer.type = wgpu::BufferBindingType::Uniform;
            e.buffer.minBindingSize = size;
            return e;
        };
        wgpu::BindGroupLayoutEntry samp{};
        samp.binding = 1;
        samp.visibility = wgpu::ShaderStage::Fragment;
        samp.sampler.type = wgpu::SamplerBindingType::Filtering;
        // The depth is read with textureLoad only, so it needs no sampler and
        // no filterable sample type.
        wgpu::BindGroupLayoutEntry depth{};
        depth.binding = 5;
        depth.visibility = wgpu::ShaderStage::Fragment;
        depth.texture.sampleType = wgpu::TextureSampleType::Depth;
        depth.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        depth.texture.multisampled = true;

        {
            wgpu::BindGroupLayoutEntry e[3] = {tex(0), samp, ub(2, sizeof(BlurUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofPassBGL";
            d.entryCount = 3;
            d.entries = e;
            mPassBGL = dev.CreateBindGroupLayout(&d);
        }
        {
            wgpu::BindGroupLayoutEntry e[5] = {samp, tex(3), tex(4), depth,
                                               ub(6, sizeof(DofUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofCompositeBGL";
            d.entryCount = 5;
            d.entries = e;
            mCompositeBGL = dev.CreateBindGroupLayout(&d);
        }
        {
            // np-look23 (c): the depth-weighted downsample (src, sampler, depth, params).
            wgpu::BindGroupLayoutEntry e[4] = {tex(0), samp, depth, ub(6, sizeof(DofUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofDownDepthWeightBGL";
            d.entryCount = 4;
            d.entries = e;
            mDownDwBGL = dev.CreateBindGroupLayout(&d);
        }
        {
            // np-look23 (c2): the half-size factor pre-pass (depth, params) and
            // the downsample that samples it (src, sampler, factor).
            wgpu::BindGroupLayoutEntry e[2] = {depth, ub(6, sizeof(DofUniforms))};
            wgpu::BindGroupLayoutDescriptor d{};
            d.label = "DofDepthWeightFactorHalfBGL";
            d.entryCount = 2;
            d.entries = e;
            mFactorHalfBGL = dev.CreateBindGroupLayout(&d);
            wgpu::BindGroupLayoutEntry e2[3] = {tex(0), samp, tex(7)};
            d.label = "DofDownDepthWeightHalfBGL";
            d.entryCount = 3;
            d.entries = e2;
            mDownDwHBGL = dev.CreateBindGroupLayout(&d);
        }
        wgpu::PipelineLayoutDescriptor pl{};
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &mPassBGL;
        mPassPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mCompositeBGL;
        mCompositePL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mDownDwBGL;
        mDownDwPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mFactorHalfBGL;
        mFactorHalfPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mDownDwHBGL;
        mDownDwHPL = dev.CreatePipelineLayout(&pl);

        wgpu::BufferDescriptor bd{};
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        bd.size = sizeof(BlurUniforms);
        bd.label = "DofBlurH";
        mBlurUB[0] = dev.CreateBuffer(&bd);
        bd.label = "DofBlurV";
        mBlurUB[1] = dev.CreateBuffer(&bd);
        bd.size = sizeof(DofUniforms);
        bd.label = "DofComposite";
        mCompositeUB = dev.CreateBuffer(&bd);
    }

    auto make = [&](const wgpu::PipelineLayout& layout, const char* fs, const char* label) {
        return MakePipe(layout, fs, label, gpu);
    };
    mDownPipe = make(mPassPL, "fs_down4", "DofDownsample4x");
    mBlurPipe = make(mPassPL, "fs_blur", "DofBlur");
    mCompositePipe = make(mCompositePL, "fs_composite", "DofComposite");
    mDownTentPipe = nullptr;           // `enhanced`: Prewarm or the first Run
    mCompositeBSplinePipe = nullptr;
    mDownDwPipe = mBlurDwPipe = mCompositeDwPipe = nullptr;
    mDownDw4Pipe = mDownDw1Pipe = mDownDwHPipe = mFactorHalfPipe = nullptr;
    // The quarter and output textures are in `format` too.
    mW = mH = 0;
}

wgpu::RenderPipeline DofPass::MakePipe(const wgpu::PipelineLayout& layout, const char* fs,
                                   const char* label, GpuDevice& gpu, wgpu::TextureFormat format) {
    auto& dev = gpu.Device();
    wgpu::ColorTargetState ct{};
    ct.format = format == wgpu::TextureFormat::Undefined ? mFormat : format;
    ct.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState frag{};
    frag.module = mShader;
    frag.entryPoint = fs;
    frag.targetCount = 1;
    frag.targets = &ct;
    wgpu::RenderPipelineDescriptor pd{};
    pd.label = label;
    pd.layout = layout;
    pd.vertex.module = mShader;
    pd.vertex.entryPoint = "vs_full";
    pd.fragment = &frag;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    return CreateCountedRenderPipeline(dev, &pd);
}

void DofPass::EnsureEnhancedPipelines(GpuDevice& gpu) {
    if (mDownTentPipe) return;
    mDownTentPipe = MakePipe(mPassPL, "fs_down_tent", "DofDownsampleTent", gpu);
    mCompositeBSplinePipe = MakePipe(mCompositePL, "fs_composite_bspline", "DofCompositeBSpline", gpu);
}

void DofPass::EnsureDepthWeightPipelines(GpuDevice& gpu) {
    if (mDownDwPipe) return;
    mDownDwPipe = MakePipe(mDownDwPL, "fs_down_tent_dw", "DofDownsampleTentDepthWeight", gpu,
                           kDepthWeightFormat);
    mBlurDwPipe = MakePipe(mPassPL, "fs_blur", "DofBlurDepthWeight", gpu, kDepthWeightFormat);
    mCompositeDwPipe = MakePipe(mCompositePL, "fs_composite_bspline_dw", "DofCompositeDepthWeight", gpu);
}

const wgpu::RenderPipeline& DofPass::DepthWeightDownPipe(int loads, GpuDevice& gpu) {
    switch (loads) {
    case kDepthWeightLoads4:
        if (!mDownDw4Pipe)
            mDownDw4Pipe = MakePipe(mDownDwPL, "fs_down_tent_dw4", "DofDownsampleTentDepthWeight4", gpu,
                                    kDepthWeightFormat);
        return mDownDw4Pipe;
    case kDepthWeightLoads1:
        if (!mDownDw1Pipe)
            mDownDw1Pipe = MakePipe(mDownDwPL, "fs_down_tent_dw1", "DofDownsampleTentDepthWeight1", gpu,
                                    kDepthWeightFormat);
        return mDownDw1Pipe;
    case kDepthWeightHalfFactor:
        if (!mFactorHalfPipe)
            mFactorHalfPipe = MakePipe(mFactorHalfPL, "fs_dw_factor_half", "DofDepthWeightFactorHalf", gpu,
                                       kDepthWeightFactorFormat);
        if (!mDownDwHPipe)
            mDownDwHPipe = MakePipe(mDownDwHPL, "fs_down_tent_dwh", "DofDownsampleTentDepthWeightHalf", gpu,
                                    kDepthWeightFormat);
        return mDownDwHPipe;
    default:
        return mDownDwPipe;
    }
}

void DofPass::EnsureDepthWeightTextures(int w, int h, GpuDevice& gpu) {
    const int qw = std::max(1, w >> 2), qh = std::max(1, h >> 2);
    const int hw = std::max(1, w >> 1), hh = std::max(1, h >> 1);
    if (mDwFactorW != hw || mDwFactorH != hh || !mDwFactorView) {
        // np-look23 (c2): the half factor's target (made with the quarters;
        // unused by the other gathers).
        if (mBindGroups && mDwFactorView) mBindGroups->Forget(mDwFactorView.Get());
        wgpu::TextureDescriptor td{};
        td.label = "DofDepthWeightFactorHalf";
        td.size = {(uint32_t)hw, (uint32_t)hh, 1};
        td.format = kDepthWeightFactorFormat;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mDwFactor = gpu.CreateTexture(td);
        mDwFactorView = mDwFactor.CreateView();
        mDwFactorW = hw;
        mDwFactorH = hh;
    }
    if (mDwW == qw && mDwH == qh && mDwView[0]) return;
    if (mBindGroups)
        for (int k = 0; k < 2; k++)
            if (mDwView[k]) mBindGroups->Forget(mDwView[k].Get());
    for (int k = 0; k < 2; k++) {
        wgpu::TextureDescriptor td{};
        td.label = k ? "DofBlurDepthWeightB" : "DofBlurDepthWeightA";
        td.size = {(uint32_t)qw, (uint32_t)qh, 1};
        td.format = kDepthWeightFormat;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mDw[k] = gpu.CreateTexture(td);
        mDwView[k] = mDw[k].CreateView();
    }
    mDwW = qw;
    mDwH = qh;
}

float DofPass::EnhancedTapScale(float v) {
    if (!(v > 0.0f)) return 1.0f;
    const float k2 = 1.0f - (3.0f / 32.0f + 1.0f / 6.0f) / v;
    return std::clamp(std::sqrt(std::max(k2, 0.0f)), 0.5f, 1.0f);
}

void DofPass::EnsureTextures(int w, int h, wgpu::TextureFormat format, GpuDevice& gpu) {
    if (mW == w && mH == h && mOutTex) return;
    auto& dev = gpu.Device();
    // NgDOFProc's constructor: blur targets are the pre-process texture >> 2.
    const uint32_t qw = (uint32_t)std::max(1, w >> 2);
    const uint32_t qh = (uint32_t)std::max(1, h >> 2);
    if (mBindGroups) {
        for (int k = 0; k < 2; k++)
            if (mQuarterView[k]) mBindGroups->Forget(mQuarterView[k].Get());
        if (mOutView) mBindGroups->Forget(mOutView.Get());
    }
    for (int k = 0; k < 2; k++) {
        wgpu::TextureDescriptor td{};
        td.label = k ? "DofBlurB" : "DofBlurA";
        td.size = {qw, qh, 1};
        td.format = format;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mQuarter[k] = gpu.CreateTexture(td);
        mQuarterView[k] = mQuarter[k].CreateView();
    }
    wgpu::TextureDescriptor od{};
    od.label = "DofOut";
    od.size = {(uint32_t)w, (uint32_t)h, 1};
    od.format = format;
    od.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc;  // CopySrc: readback in tests
    mOutTex = gpu.CreateTexture(od);
    mOutView = mOutTex.CreateView();
    mW = w;
    mH = h;
}

void DofPass::Pass(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                   const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label) {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = label;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    MILO_GPU_TIME_PASS(rp);
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

bool DofPass::Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                  wgpu::TextureFormat sceneFormat, const wgpu::TextureView& depthView,
                  uint32_t depthSamples, int sceneW, int sceneH, GpuDevice& gpu, bool enhanced,
                  bool depthWeighted, int depthWeightLoads) {
    extern DOFProc* TheDOFProc;
    if (!TheDOFProc || !TheDOFProc->Enabled()) return false;
    if (!sceneView || !depthView || sceneW <= 0 || sceneH <= 0) return false;
    if (depthSamples < 2) {
        // The composite binds the depth as texture_depth_multisampled_2d.
        if (!mWarnedSamples) {
            fprintf(stderr, "DofPass: single-sample depth is not supported; depth of field off\n");
            mWarnedSamples = true;
        }
        return false;
    }
    EnsurePipelines(sceneFormat, gpu);
    EnsureTextures(sceneW, sceneH, sceneFormat, gpu);
    auto& dev = gpu.Device();
    auto& queue = gpu.Queue();

    const float widthScale = RndPostProc::DOFOverrides().mBlurWidthScale;
    // Render scale: the offsets keep the output size's quarter (SceneBlurOffsets);
    // the enhanced spread below is in the quarter targets' own texels (qw, qh).
    const int qw = std::max(1, sceneW >> 2);
    const int qh = std::max(1, sceneH >> 2);
    const float rs = gpu.RenderScaleFor(sceneW);
    float off[2][8][2];
    for (int k = 0; k < 2; k++) SceneBlurOffsets(k == 1, sceneW, sceneH, rs, widthScale, off[k]);
    float tapScale[2] = {1.0f, 1.0f};
    if (enhanced) {
        // Warmed at Init when the look starts with postDofFilter on
        // (Prewarm); a runtime switch to it compiles them here, once.
        EnsureEnhancedPipelines(gpu);
        if (depthWeighted) {
            EnsureDepthWeightPipelines(gpu);
            EnsureDepthWeightTextures(sceneW, sceneH, gpu);
        }
        // The two passes' spread per axis, in quarter texels^2 (each pass the
        // mean of its 8 taps, so its variance is E[o^2] - E[o]^2).
        for (int a = 0; a < 2; a++) {
            const float texels = (float)(a ? qh : qw);
            float v = 0.0f;
            for (int k = 0; k < 2; k++) {
                float m = 0.0f, m2 = 0.0f;
                for (int i = 0; i < 8; i++) {
                    const float o = off[k][i][a] * texels;
                    m += o / 8.0f;
                    m2 += o * o / 8.0f;
                }
                v += m2 - m * m;
            }
            tapScale[a] = EnhancedTapScale(v);
        }
    }
    for (int k = 0; k < 2; k++) {
        BlurUniforms bu{};
        for (int i = 0; i < 8; i++) {
            bu.taps[i][0] = off[k][i][0] * tapScale[0];
            bu.taps[i][1] = off[k][i][1] * tapScale[1];
            bu.taps[i][2] = 1.0f;
            bu.taps[i][3] = 1.0f;
        }
        queue.WriteBuffer(mBlurUB[k], 0, &bu, sizeof(bu));
    }

    // The planes of the camera the shot gave NgDOFProc::Set (DC3), else the
    // current camera's (RB3, or a Set with no camera).
    float nearPlane = 1.0f, farPlane = 1000.0f;
    Vector2 zr(0.0f, 1.0f);
    if (!rndshape::DofSetCamPlanes(nearPlane, farPlane, zr)) {
        if (RndCam* cam = RndCam::Current()) {
            nearPlane = cam->NearPlane();
            farPlane = cam->FarPlane();
            zr = rndshape::CamZRange(cam);
        }
    }
    DofUniforms du{};
    RetailConstants(TheDOFProc->FocalPlane(), TheDOFProc->BlurDepth(), TheDOFProc->MaxBlur(),
                    TheDOFProc->MinBlur(), nearPlane, farPlane, zr.x, zr.y, du.c24);
    for (int i = 0; i < 4; i++) mLastConstants[i] = du.c24[i];
    queue.WriteBuffer(mCompositeUB, 0, &du, sizeof(du));

    auto passBG = [&](const wgpu::TextureView& src, const wgpu::Buffer& ub) {
        wgpu::BindGroupEntry e[3] = {};
        e[0].binding = 0;
        e[0].textureView = src;
        e[1].binding = 1;
        e[1].sampler = mSampler;
        e[2].binding = 2;
        e[2].buffer = ub;
        e[2].size = sizeof(BlurUniforms);
        wgpu::BindGroupDescriptor d{};
        d.layout = mPassBGL;
        d.entryCount = 3;
        d.entries = e;
        return Group(dev, d);
    };

    if (depthWeightLoads != kDepthWeightLoads4 && depthWeightLoads != kDepthWeightLoads1 &&
        depthWeightLoads != kDepthWeightHalfFactor)
        depthWeightLoads = kDepthWeightLoads16;
    const bool dw = enhanced && depthWeighted;
    mLastDepthWeighted = dw;
    if (dw) mLastDwLoads = depthWeightLoads;
    // The quarter targets: retail's (scene format), or the depth-weighted
    // path's RGBA16Float pair.
    const wgpu::TextureView* q = dw ? mDwView : mQuarterView;
    // 1. downsample_4x: scene -> A.  2. blur, horizontal taps: A -> B.
    // 3. blur, vertical taps: B -> A.
    if (dw && depthWeightLoads == kDepthWeightHalfFactor) {
        // np-look23 (c2): the half factor, then the downsample that samples it.
        const wgpu::RenderPipeline& down = DepthWeightDownPipe(depthWeightLoads, gpu);
        {
            wgpu::BindGroupEntry e[2] = {};
            e[0].binding = 5;
            e[0].textureView = depthView;
            e[1].binding = 6;
            e[1].buffer = mCompositeUB;
            e[1].size = sizeof(DofUniforms);
            wgpu::BindGroupDescriptor d{};
            d.layout = mFactorHalfBGL;
            d.entryCount = 2;
            d.entries = e;
            Pass(encoder, mFactorHalfPipe, Group(dev, d), mDwFactorView, "DofDepthWeightFactorHalf");
        }
        {
            wgpu::BindGroupEntry e[3] = {};
            e[0].binding = 0;
            e[0].textureView = sceneView;
            e[1].binding = 1;
            e[1].sampler = mSampler;
            e[2].binding = 7;
            e[2].textureView = mDwFactorView;
            wgpu::BindGroupDescriptor d{};
            d.layout = mDownDwHBGL;
            d.entryCount = 3;
            d.entries = e;
            Pass(encoder, down, Group(dev, d), q[0], "DofDownsampleTentDepthWeightHalf");
        }
        Pass(encoder, mBlurDwPipe, passBG(q[0], mBlurUB[0]), q[1], "DofBlurH");
        Pass(encoder, mBlurDwPipe, passBG(q[1], mBlurUB[1]), q[0], "DofBlurV");
    } else if (dw) {
        // The pass names carry the gather (16: today's name) for the GPU timers.
        const char* downLabel = depthWeightLoads == kDepthWeightLoads4 ? "DofDownsampleTentDepthWeight4"
                              : depthWeightLoads == kDepthWeightLoads1 ? "DofDownsampleTentDepthWeight1"
                                                                       : "DofDownsampleTentDepthWeight";
        const wgpu::RenderPipeline& down = DepthWeightDownPipe(depthWeightLoads, gpu);
        wgpu::BindGroupEntry e[4] = {};
        e[0].binding = 0;
        e[0].textureView = sceneView;
        e[1].binding = 1;
        e[1].sampler = mSampler;
        e[2].binding = 5;
        e[2].textureView = depthView;
        e[3].binding = 6;
        e[3].buffer = mCompositeUB;
        e[3].size = sizeof(DofUniforms);
        wgpu::BindGroupDescriptor d{};
        d.layout = mDownDwBGL;
        d.entryCount = 4;
        d.entries = e;
        Pass(encoder, down, Group(dev, d), q[0], downLabel);
        Pass(encoder, mBlurDwPipe, passBG(q[0], mBlurUB[0]), q[1], "DofBlurH");
        Pass(encoder, mBlurDwPipe, passBG(q[1], mBlurUB[1]), q[0], "DofBlurV");
    } else if (enhanced) {
        Pass(encoder, mDownTentPipe, passBG(sceneView, mBlurUB[0]), mQuarterView[0], "DofDownsampleTent");
    } else {
        Pass(encoder, mDownPipe, passBG(sceneView, mBlurUB[0]), mQuarterView[0], "DofDownsample4x");
    }
    if (!dw) {
        Pass(encoder, mBlurPipe, passBG(mQuarterView[0], mBlurUB[0]), mQuarterView[1], "DofBlurH");
        Pass(encoder, mBlurPipe, passBG(mQuarterView[1], mBlurUB[1]), mQuarterView[0], "DofBlurV");
    }

    // 4. The composite: scene, A, depth -> output.
    {
        wgpu::BindGroupEntry e[5] = {};
        e[0].binding = 1;
        e[0].sampler = mSampler;
        e[1].binding = 3;
        e[1].textureView = sceneView;
        e[2].binding = 4;
        e[2].textureView = q[0];
        e[3].binding = 5;
        e[3].textureView = depthView;
        e[4].binding = 6;
        e[4].buffer = mCompositeUB;
        e[4].size = sizeof(DofUniforms);
        wgpu::BindGroupDescriptor d{};
        d.layout = mCompositeBGL;
        d.entryCount = 5;
        d.entries = e;
        if (dw) {
            Pass(encoder, mCompositeDwPipe, Group(dev, d), mOutView, "DofCompositeDepthWeight");
        } else if (enhanced) {
            Pass(encoder, mCompositeBSplinePipe, Group(dev, d), mOutView, "DofCompositeBSpline");
        } else {
            Pass(encoder, mCompositePipe, Group(dev, d), mOutView, "DofComposite");
        }
    }
    return true;
}

void DofPass::Terminate() {
    mShader = nullptr;
    mPassBGL = nullptr;
    mCompositeBGL = nullptr;
    mPassPL = nullptr;
    mCompositePL = nullptr;
    mDownPipe = nullptr;
    mBlurPipe = nullptr;
    mCompositePipe = nullptr;
    mDownTentPipe = nullptr;
    mCompositeBSplinePipe = nullptr;
    mDownDwPipe = mBlurDwPipe = mCompositeDwPipe = nullptr;
    mDownDwBGL = nullptr;
    mDownDwPL = nullptr;
    mDownDw4Pipe = mDownDw1Pipe = mDownDwHPipe = mFactorHalfPipe = nullptr;
    mFactorHalfBGL = mDownDwHBGL = nullptr;
    mFactorHalfPL = mDownDwHPL = nullptr;
    mDwFactor = nullptr;
    mDwFactorView = nullptr;
    mDwFactorW = mDwFactorH = 0;
    mLastDwLoads = kDepthWeightLoads16;
    mDw[0] = mDw[1] = nullptr;
    mDwView[0] = mDwView[1] = nullptr;
    mDwW = mDwH = 0;
    mSampler = nullptr;
    mBlurUB[0] = mBlurUB[1] = nullptr;
    mCompositeUB = nullptr;
    mQuarter[0] = mQuarter[1] = nullptr;
    mQuarterView[0] = mQuarterView[1] = nullptr;
    mOutTex = nullptr;
    mOutView = nullptr;
    mW = mH = 0;
    mFormat = wgpu::TextureFormat::Undefined;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* DofPassWgslSource() { return kDofShaderSource; }
