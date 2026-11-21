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
    float c24[4];   // range, -scale * range, minBlur, maxBlur
    float near[4];  // np-look36: x = the near field's spread over the focus (0 / 1)
};
static_assert(sizeof(DofUniforms) == 32, "DofUniforms layout");

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
    near: vec4f,  // np-look36: x = the near field's spread (fs_composite_bspline_dwn)
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
// same 8x8 tent, each tap weighted by the blur factor squared there,
// premultiplied, the weight sum in alpha. An in-focus texel (factor ~0) then
// adds nothing to the blur, so the blurred background next to an in-focus
// figure is background only: retail's depth-blind downsample spreads the
// figure's colour into it, a bright fringe ~12 px wide at 720 (Xenia and the
// hardware show it too). The blur passes average the weighted colour and the
// weight alike; the composite divides (fs_composite_bspline_dw). The quarter
// targets are RGBA16Float here, so the division does not amplify 8-bit
// rounding.
fn dof_factor(z: f32) -> f32 {
    let t = z * dof.c24.x + dof.c24.y;
    return clamp(min(max(abs(t), dof.c24.z), dof.c24.w), 0.0, 1.0);
}
// np-look36: the near field, out of focus and IN FRONT of the focal plane
// (t < 0): 0 up to |t| = 0.2, 1 from 0.6. Exactly 0 at and behind the focus, so
// the near rule leaves the far field and the in-focus figures to the pixel.
fn dof_near(z: f32) -> f32 {
    let t = z * dof.c24.x + dof.c24.y;
    return smoothstep(0.2, 0.6, -t);
}
// np-look51, LookSettings::postDofEdge (the edge rule; Run's `edge`, the
// ..._edge composites below). The scene the composites mix is the resolved
// colour, so a pixel an in-focus figure half covers already holds its
// anti-aliased blend; the mix amount came from depth sample 0 alone, so such a
// pixel was kept whole or replaced whole by the blur and the outline stepped
// at one sample per pixel. The rule reads every sample: the mix amount is the
// smallest factor of all the samples (an in-focus sample keeps the pixel's
// blend), raised to the largest factor of the samples in front of the focus
// (t < 0, dof_near's side: a pixel a blurred near object touches stays
// blurred, so its soft edge stays soft); no sample in front counts 0.
// The smallest is taken over ALL the samples, not only those at or behind the
// focus: an in-focus figure is in front of the focal plane by a little as
// often as behind it, and a figure sample in front (small factor) over the far
// background would otherwise leave the background's factor alone in the
// smallest's group and replace the pixel. x: that mix amount; y: the near
// field, the largest of the samples' dof_near. A pixel whose samples share one
// depth gets dof_factor / dof_near of that depth.
fn dof_edge(p: vec2i) -> vec2f {
    let q = min(p, vec2i(textureDimensions(depthTex)) - 1);
    let count = i32(textureNumSamples(depthTex));
    var lowest = 1.0;
    var front = 0.0;
    var nearMax = 0.0;
    for (var i = 0; i < count; i++) {
        let z = textureLoad(depthTex, q, i);
        let t = z * dof.c24.x + dof.c24.y;
        let f = dof_factor(z);
        lowest = min(lowest, f);
        if (t < 0.0) {
            front = max(front, f);
        }
        nearMax = max(nearMax, dof_near(z));
    }
    return vec2f(max(lowest, front), nearMax);
}
// np-look23 (c2): the weight comes from a half-size pre-pass. It writes the
// factor squared from one depth load per half-size texel (scene texel 2p + 1:
// each multisampled depth texel read at most once, the reads coherent) into
// an R16Float target, and the downsample samples that bilinearly at each of
// its 16 taps (factor texel p under scene texels 2p and 2p + 1, read at the
// tap's scene position / 2, so an odd size maps exactly). One depth load per tap in the downsample itself (16 per quarter
// texel, scattered) cost +0.84 ms at 1920x1080 on a Radeon 890M; this costs
// +0.25 ms there.
@fragment fn fs_dw_factor_half(in: VOut) -> @location(0) vec4f {
    let dmax = vec2i(textureDimensions(depthTex)) - 1;
    let p = vec2i(in.pos.xy) * 2 + 1;
    let f = dof_factor(textureLoad(depthTex, min(p, dmax), 0));
    return vec4f(f * f, 0.0, 0.0, 1.0);
}
// np-look36: the same pre-pass with the near field in G, into an RG16Float
// target (DofDepthWeightFactorHalfNear). Only the near field's passes read G.
@fragment fn fs_dw_factor_half_near(in: VOut) -> @location(0) vec4f {
    let dmax = vec2i(textureDimensions(depthTex)) - 1;
    let p = vec2i(in.pos.xy) * 2 + 1;
    let z = textureLoad(depthTex, min(p, dmax), 0);
    let f = dof_factor(z);
    return vec4f(f * f, dof_near(z), 0.0, 1.0);
}
@group(0) @binding(7) var factorTex: texture_2d<f32>;
@fragment fn fs_down_tent_dwh(in: VOut) -> @location(0) vec4f {
    let size = vec2f(textureDimensions(srcTex));
    let fsize = vec2f(textureDimensions(factorTex)) * 2.0;
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let q = c + vec2f(kTentO[i], kTentO[j]);
            let w = kTentW[i] * kTentW[j] * textureSampleLevel(factorTex, samp, q / fsize, 0.0).r;
            acc += vec4f(textureSampleLevel(srcTex, samp, q / size, 0.0).rgb * w, w);
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
// np-look51: the same with the edge rule's mix amount (dof_edge).
@fragment fn fs_composite_bspline_dw_edge(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let b = blur_bspline(in.uv);
    let rgb = mix(scene.rgb, b.rgb / max(b.a, 1e-4), clamp(b.a * 16.0, 0.0, 1.0));
    let f = dof_edge(p).x;
    return vec4f(clamp(mix(scene.rgb, rgb, f), vec3f(0.0), vec3f(1.0)), scene.a);
}

// np-look36, LookSettings::postDofNearField: the depth weight keeps in-focus
// colour out of EVERY blurred pixel, which is right behind the focus (the
// background beside a dancer) and wrong in front of it: a blurred near object
// is see-through at its edge, so the sharp figure behind it belongs in its
// blur, and its blur spills over the figure beyond its silhouette. The
// downsample writes a second quarter target beside the weighted one: the
// plain tent (depth-blind, as retail's) with the near-field coverage in
// alpha (the factor target's G, dof_near). Same 16 + 16 reads as
// fs_down_tent_dwh, one more target.
struct DownNearOut {
    @location(0) weighted: vec4f,
    @location(1) near: vec4f,
};
@fragment fn fs_down_tent_dwn(in: VOut) -> DownNearOut {
    let size = vec2f(textureDimensions(srcTex));
    let fsize = vec2f(textureDimensions(factorTex)) * 2.0;
    let c = floor(in.pos.xy) * 4.0 + 2.0;
    var acc = vec4f(0.0);
    var accN = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let q = c + vec2f(kTentO[i], kTentO[j]);
            let k = kTentW[i] * kTentW[j];
            let fw = textureSampleLevel(factorTex, samp, q / fsize, 0.0).rg;
            let rgb = textureSampleLevel(srcTex, samp, q / size, 0.0).rgb;
            acc += vec4f(rgb * (k * fw.r), k * fw.r);
            accN += k * vec4f(rgb, fw.g);
        }
    }
    var out: DownNearOut;
    out.weighted = acc;
    out.near = accN;
    return out;
}
// Both quarter targets through the same 8 taps (+8 reads per quarter texel).
@group(0) @binding(8) var src2Tex: texture_2d<f32>;
@fragment fn fs_blur_near(in: VOut) -> DownNearOut {
    var acc = vec4f(0.0);
    var accN = vec4f(0.0);
    for (var i = 0; i < 8; i++) {
        acc += textureSampleLevel(srcTex, samp, in.uv + blur.taps[i].xy, 0.0);
        accN += textureSampleLevel(src2Tex, samp, in.uv + blur.taps[i].xy, 0.0);
    }
    var out: DownNearOut;
    out.weighted = acc * 0.125;
    out.near = accN * 0.125;
    return out;
}
// The composite: a pixel in the near field takes the depth-blind blur (the
// figure behind shows through its edge, at least as retail's does); with
// near.x the near field's blurred coverage also mixes the weighted blur (the
// near object's colour, never the in-focus figure's) over the pixels beside
// it. Where the coverage and the centre's own near value are 0 -- the far
// field, the in-focus figures away from a near object -- this is
// fs_composite_bspline_dw to the pixel. +1 read per pixel (the coverage,
// with the spread), +4 more in the near field.
@group(0) @binding(9) var nearTex: texture_2d<f32>;
@fragment fn fs_composite_bspline_dwn(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let b = blur_bspline(in.uv);
    let rgbW = mix(scene.rgb, b.rgb / max(b.a, 1e-4), clamp(b.a * 16.0, 0.0, 1.0));
    let z = textureLoad(depthTex, min(p, vec2i(textureDimensions(depthTex)) - 1), 0);
    let f = dof_factor(z);
    let n = dof_near(z);
    // The depth-blind colour only where the centre is in the near field (a
    // branch: 4 reads there, none elsewhere); the spread's coverage from one
    // bilinear read everywhere (the coverage is itself blurred). +1 read per
    // pixel outside the near field.
    var rgb = rgbW;
    if (n > 0.0) {
        rgb = mix(rgbW, blur_bspline_near(in.uv).rgb, n);
    }
    var a = f;
    if (dof.near.x > 0.0) {
        a = max(f, clamp(textureSampleLevel(nearTex, samp, in.uv, 0.0).a, 0.0, 1.0));
    }
    return vec4f(clamp(mix(scene.rgb, rgb, a), vec3f(0.0), vec3f(1.0)), scene.a);
}
// np-look51: the same with the edge rule (dof_edge): its mix amount, and the
// near field the largest of the samples', so every pixel a near object
// touches takes the depth-blind colour.
@fragment fn fs_composite_bspline_dwn_edge(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let b = blur_bspline(in.uv);
    let rgbW = mix(scene.rgb, b.rgb / max(b.a, 1e-4), clamp(b.a * 16.0, 0.0, 1.0));
    let e = dof_edge(p);
    let f = e.x;
    let n = e.y;
    var rgb = rgbW;
    if (n > 0.0) {
        rgb = mix(rgbW, blur_bspline_near(in.uv).rgb, n);
    }
    var a = f;
    if (dof.near.x > 0.0) {
        a = max(f, clamp(textureSampleLevel(nearTex, samp, in.uv, 0.0).a, 0.0, 1.0));
    }
    return vec4f(clamp(mix(scene.rgb, rgb, a), vec3f(0.0), vec3f(1.0)), scene.a);
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

fn blur_bspline_near(uv: vec2f) -> vec4f {
    let size = vec2f(textureDimensions(nearTex));
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
    return g0.x * g0.y * textureSampleLevel(nearTex, samp, p0, 0.0) +
           g1.x * g0.y * textureSampleLevel(nearTex, samp, vec2f(p1.x, p0.y), 0.0) +
           g0.x * g1.y * textureSampleLevel(nearTex, samp, vec2f(p0.x, p1.y), 0.0) +
           g1.x * g1.y * textureSampleLevel(nearTex, samp, p1, 0.0);
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
// np-look51: the enhanced composite without the depth weight (reached only by
// {rnd native_look dof_depth_weight_off}) with the edge rule (dof_edge).
@fragment fn fs_composite_bspline_edge(in: VOut) -> @location(0) vec4f {
    let p = vec2i(in.pos.xy);
    let scene = textureLoad(sceneTex, p, 0);
    let f = dof_edge(p).x;
    return clamp(mix(scene, blur_bspline(in.uv), f), vec4f(0.0), vec4f(1.0));
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
            // np-look23 (c): the depth weight's half-size factor pre-pass (depth, params) and
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
            mDownDwBGL = dev.CreateBindGroupLayout(&d);
            // np-look36: the near field's blur (two sources) and composite
            // (the near quarter at 9).
            wgpu::BindGroupLayoutEntry e3[4] = {tex(0), samp, ub(2, sizeof(BlurUniforms)), tex(8)};
            d.label = "DofBlurNearBGL";
            d.entryCount = 4;
            d.entries = e3;
            mBlurNearBGL = dev.CreateBindGroupLayout(&d);
            wgpu::BindGroupLayoutEntry e4[6] = {samp, tex(3), tex(4), depth, ub(6, sizeof(DofUniforms)),
                                                tex(9)};
            d.label = "DofCompositeNearBGL";
            d.entryCount = 6;
            d.entries = e4;
            mCompositeNearBGL = dev.CreateBindGroupLayout(&d);
        }
        wgpu::PipelineLayoutDescriptor pl{};
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &mPassBGL;
        mPassPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mCompositeBGL;
        mCompositePL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mFactorHalfBGL;
        mFactorHalfPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mDownDwBGL;
        mDownDwPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mBlurNearBGL;
        mBlurNearPL = dev.CreatePipelineLayout(&pl);
        pl.bindGroupLayouts = &mCompositeNearBGL;
        mCompositeNearPL = dev.CreatePipelineLayout(&pl);

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
    mFactorHalfPipe = mDownDwPipe = mBlurDwPipe = mCompositeDwPipe = nullptr;
    mFactorHalfNearPipe = mDownNearPipe = mBlurNearPipe = mCompositeNearPipe = nullptr;
    mCompositeBSplineEdgePipe = mCompositeDwEdgePipe = mCompositeNearEdgePipe = nullptr;
    // The quarter and output textures are in `format` too.
    mW = mH = 0;
}

wgpu::RenderPipeline DofPass::MakePipe(const wgpu::PipelineLayout& layout, const char* fs,
                                   const char* label, GpuDevice& gpu, wgpu::TextureFormat format,
                                   uint32_t targets) {
    auto& dev = gpu.Device();
    wgpu::ColorTargetState ct[2]{};
    for (auto& t : ct) {
        t.format = format == wgpu::TextureFormat::Undefined ? mFormat : format;
        t.writeMask = wgpu::ColorWriteMask::All;
    }
    wgpu::FragmentState frag{};
    frag.module = mShader;
    frag.entryPoint = fs;
    frag.targetCount = targets;
    frag.targets = ct;
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
    mFactorHalfPipe = MakePipe(mFactorHalfPL, "fs_dw_factor_half", "DofDepthWeightFactorHalf", gpu,
                               kDepthWeightFactorFormat);
    mDownDwPipe = MakePipe(mDownDwPL, "fs_down_tent_dwh", "DofDownsampleTentDepthWeightHalf", gpu,
                           kDepthWeightFormat);
    mBlurDwPipe = MakePipe(mPassPL, "fs_blur", "DofBlurDepthWeight", gpu, kDepthWeightFormat);
    mCompositeDwPipe = MakePipe(mCompositePL, "fs_composite_bspline_dw", "DofCompositeDepthWeight", gpu);
}

void DofPass::EnsureNearPipelines(GpuDevice& gpu) {
    if (mDownNearPipe) return;
    mFactorHalfNearPipe = MakePipe(mFactorHalfPL, "fs_dw_factor_half_near", "DofDepthWeightFactorHalfNear",
                                   gpu, kDepthWeightFactorNearFormat);
    mDownNearPipe = MakePipe(mDownDwPL, "fs_down_tent_dwn", "DofDownsampleTentNear", gpu,
                             kDepthWeightFormat, 2);
    mBlurNearPipe = MakePipe(mBlurNearPL, "fs_blur_near", "DofBlurNear", gpu, kDepthWeightFormat, 2);
    mCompositeNearPipe = MakePipe(mCompositeNearPL, "fs_composite_bspline_dwn", "DofCompositeNear", gpu);
}

void DofPass::EnsureEdgePipelines(GpuDevice& gpu, bool depthWeighted, bool nearField) {
    // np-look51: the edge rule's composite for the path that runs, each made
    // on first use (or Prewarm); the composites without it are untouched.
    if (nearField) {
        if (!mCompositeNearEdgePipe)
            mCompositeNearEdgePipe =
                MakePipe(mCompositeNearPL, "fs_composite_bspline_dwn_edge", "DofCompositeNearEdge", gpu);
    } else if (depthWeighted) {
        if (!mCompositeDwEdgePipe)
            mCompositeDwEdgePipe =
                MakePipe(mCompositePL, "fs_composite_bspline_dw_edge", "DofCompositeDepthWeightEdge", gpu);
    } else if (!mCompositeBSplineEdgePipe) {
        mCompositeBSplineEdgePipe =
            MakePipe(mCompositePL, "fs_composite_bspline_edge", "DofCompositeBSplineEdge", gpu);
    }
}

void DofPass::EnsureNearTextures(int w, int h, GpuDevice& gpu) {
    const int qw = std::max(1, w >> 2), qh = std::max(1, h >> 2);
    if (mNearW == qw && mNearH == qh && mNearView[0]) return;
    if (mBindGroups)
        for (int k = 0; k < 2; k++)
            if (mNearView[k]) mBindGroups->Forget(mNearView[k].Get());
    for (int k = 0; k < 2; k++) {
        wgpu::TextureDescriptor td{};
        td.label = k ? "DofBlurNearB" : "DofBlurNearA";
        td.size = {(uint32_t)qw, (uint32_t)qh, 1};
        td.format = kDepthWeightFormat;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mNear[k] = gpu.CreateTexture(td);
        mNearView[k] = mNear[k].CreateView();
    }
    mNearW = qw;
    mNearH = qh;
}

void DofPass::EnsureDepthWeightTextures(int w, int h, GpuDevice& gpu, bool nearField) {
    const int qw = std::max(1, w >> 2), qh = std::max(1, h >> 2);
    // Rounded up: an odd width or height keeps its last scene column / row
    // under a factor texel of its own (fs_down_tent_dwh reads it in scene
    // texels, so the mapping is exact at any size).
    const int hw = std::max(1, (w + 1) >> 1), hh = std::max(1, (h + 1) >> 1);
    if (mDwFactorW != hw || mDwFactorH != hh || !mDwFactorView || mDwFactorNear != nearField) {
        // np-look23 (c2): the half factor's target (made with the quarters;
        // unused by the other gathers). One channel, or two with the near
        // field (np-look36): switching the near field remakes it, once.
        if (mBindGroups && mDwFactorView) mBindGroups->Forget(mDwFactorView.Get());
        wgpu::TextureDescriptor td{};
        td.label = "DofDepthWeightFactorHalf";
        td.size = {(uint32_t)hw, (uint32_t)hh, 1};
        td.format = nearField ? kDepthWeightFactorNearFormat : kDepthWeightFactorFormat;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        mDwFactor = gpu.CreateTexture(td);
        mDwFactorView = mDwFactor.CreateView();
        mDwFactorW = hw;
        mDwFactorH = hh;
        mDwFactorNear = nearField;
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
                   const wgpu::BindGroup& bg, const wgpu::TextureView& dst, const char* label,
                   const wgpu::TextureView* dst2) {
    wgpu::RenderPassColorAttachment ca[2]{};
    for (int k = 0; k < 2; k++) {
        ca[k].view = k ? (dst2 ? *dst2 : dst) : dst;
        ca[k].loadOp = wgpu::LoadOp::Clear;
        ca[k].storeOp = wgpu::StoreOp::Store;
        ca[k].clearValue = {0, 0, 0, 0};
    }
    wgpu::RenderPassDescriptor rp{};
    rp.label = label;
    rp.colorAttachmentCount = dst2 ? 2 : 1;
    rp.colorAttachments = ca;
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
                  bool depthWeighted, bool nearField, bool nearSpread, bool edge) {
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
            EnsureDepthWeightTextures(sceneW, sceneH, gpu, nearField);
            if (nearField) {
                EnsureNearPipelines(gpu);
                EnsureNearTextures(sceneW, sceneH, gpu);
            }
        }
        if (edge) EnsureEdgePipelines(gpu, depthWeighted, depthWeighted && nearField);
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
    du.near[0] = nearSpread ? 1.0f : 0.0f;
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

    const bool dw = enhanced && depthWeighted;
    const bool nf = dw && nearField;
    mLastDepthWeighted = dw;
    mLastNearField = nf;
    // np-look51: the edge rule rides on the enhanced composites only; the
    // retail one (fs_composite) never reads it.
    const bool eg = enhanced && edge;
    mLastEdge = eg;
    // The quarter targets: retail's (scene format), or the depth-weighted
    // path's RGBA16Float pair.
    const wgpu::TextureView* q = dw ? mDwView : mQuarterView;
    // 1. downsample_4x: scene -> A.  2. blur, horizontal taps: A -> B.
    // 3. blur, vertical taps: B -> A.
    if (dw) {
        // np-look23 (c): the half-size factor, then the downsample that
        // samples it.
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
            if (nf)
                Pass(encoder, mFactorHalfNearPipe, Group(dev, d), mDwFactorView, "DofDepthWeightFactorHalfNear");
            else
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
            d.layout = mDownDwBGL;
            d.entryCount = 3;
            d.entries = e;
            if (nf)
                Pass(encoder, mDownNearPipe, Group(dev, d), q[0], "DofDownsampleTentNear", &mNearView[0]);
            else
                Pass(encoder, mDownDwPipe, Group(dev, d), q[0], "DofDownsampleTentDepthWeightHalf");
        }
        if (nf) {
            // np-look36: both quarters through the same taps.
            auto nearBG = [&](int src, const wgpu::Buffer& ub) {
                wgpu::BindGroupEntry e[4] = {};
                e[0].binding = 0;
                e[0].textureView = q[src];
                e[1].binding = 1;
                e[1].sampler = mSampler;
                e[2].binding = 2;
                e[2].buffer = ub;
                e[2].size = sizeof(BlurUniforms);
                e[3].binding = 8;
                e[3].textureView = mNearView[src];
                wgpu::BindGroupDescriptor d{};
                d.layout = mBlurNearBGL;
                d.entryCount = 4;
                d.entries = e;
                return Group(dev, d);
            };
            Pass(encoder, mBlurNearPipe, nearBG(0, mBlurUB[0]), q[1], "DofBlurH", &mNearView[1]);
            Pass(encoder, mBlurNearPipe, nearBG(1, mBlurUB[1]), q[0], "DofBlurV", &mNearView[0]);
        } else {
            Pass(encoder, mBlurDwPipe, passBG(q[0], mBlurUB[0]), q[1], "DofBlurH");
            Pass(encoder, mBlurDwPipe, passBG(q[1], mBlurUB[1]), q[0], "DofBlurV");
        }
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
        wgpu::BindGroupEntry e[6] = {};
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
        if (nf) {
            e[5].binding = 9;
            e[5].textureView = mNearView[0];
            d.layout = mCompositeNearBGL;
            d.entryCount = 6;
            if (eg)
                Pass(encoder, mCompositeNearEdgePipe, Group(dev, d), mOutView, "DofCompositeNearEdge");
            else
                Pass(encoder, mCompositeNearPipe, Group(dev, d), mOutView, "DofCompositeNear");
        } else if (dw) {
            if (eg)
                Pass(encoder, mCompositeDwEdgePipe, Group(dev, d), mOutView, "DofCompositeDepthWeightEdge");
            else
                Pass(encoder, mCompositeDwPipe, Group(dev, d), mOutView, "DofCompositeDepthWeight");
        } else if (enhanced) {
            if (eg)
                Pass(encoder, mCompositeBSplineEdgePipe, Group(dev, d), mOutView, "DofCompositeBSplineEdge");
            else
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
    mFactorHalfPipe = mDownDwPipe = mBlurDwPipe = mCompositeDwPipe = nullptr;
    mFactorHalfBGL = mDownDwBGL = nullptr;
    mFactorHalfPL = mDownDwPL = nullptr;
    mFactorHalfNearPipe = mDownNearPipe = mBlurNearPipe = mCompositeNearPipe = nullptr;
    mCompositeBSplineEdgePipe = mCompositeDwEdgePipe = mCompositeNearEdgePipe = nullptr;
    mBlurNearBGL = mCompositeNearBGL = nullptr;
    mBlurNearPL = mCompositeNearPL = nullptr;
    mNear[0] = mNear[1] = nullptr;
    mNearView[0] = mNearView[1] = nullptr;
    mNearW = mNearH = 0;
    mDwFactor = nullptr;
    mDwFactorView = nullptr;
    mDwFactorW = mDwFactorH = 0;
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
