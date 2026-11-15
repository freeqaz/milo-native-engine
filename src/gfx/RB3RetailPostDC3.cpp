// RB3RetailPostDC3 — np-gfx-post: Dance Central 3's retail post chain on
// RB3RetailPost's bloom targets (RetailPostParams::dc3; see RB3RetailPost.h).
//
// Sources (all MEASURED, read from the shipped `xbox_shaders` microcode with
// xenia-gpu-shader-compiler, and from dc3-decomp's matched NgPostProc code):
//   bloom.ps         4 bilinear taps at uv +- 2 source texels (c15 = the
//                    source's texel size; c255 = (0.25, -2)), each rgb * a,
//                    averaged. Into w/4 that covers each 4x4 block exactly
//                    (RB3RetailPost's fs_bloom_down); into w/16 (the streak
//                    set1) it samples a quarter of each 16x16 block.
//   blur.ps (streak) 7 taps, SetBloomBlurWeightsStreak(horizontal, w, h,
//                    attenuation, pass, angle), dc3-decomp rndobj/Utl.cpp:
//                    weight[3] = 1/3, weight[3 +- i] = a^i / 3 with
//                    a = attenuation^(4^pass); offset 0.5 +- i * 4^pass texels;
//                    rotated by `angle`, the horizontal x scaled by the screen's
//                    height/width. The 0.5 is kept: DxRnd::DrawRect sets
//                    HalfPixelOffset, so it is a real half-texel step per
//                    pass, not a convention fix (engine doc §37.10).
//   postprocess.ps   bit 4 bloom (tf7 + tf11 + tf15, c6), bit 15 chroma (c121),
//                    bit 43 chroma sharpen, bit 2 noise (tf13, c112, c113), bit
//                    47 midtone noise, bit 21 colour matrix (c92..c94), bit 36
//                    vignette (c123). Bit numbers from RndShaderPostProc::
//                    CalcShaderOpts (dc3-decomp rndobj/Shader.cpp).
#include "gfx/RB3RetailPost.h"
#include "gfx/GpuTimers.h"
#include "gfx/GpuDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "gfx/PipelineCount.h"

namespace {

// Up to seven taps: xy = uv offset, z = weight.
struct TapUniforms {
    float tap[7][4];
};
static_assert(sizeof(TapUniforms) == 112, "TapUniforms layout");

struct DC3CompositeUniforms {
    float xfm[3][4];
    float bloomColor[4];   // rgb = c6, w: 0 off, 1 bloom, 2 show mask, 3 show bloom term
    float vignette[4];     // c123: rgb colour, w intensity (0: off)
    float chroma[4];       // c121.xy = offset / (W, H); z = 1 on; w = 1 sharpen
    float noiseSeeds[4];   // c112
    float noiseParams[4];  // c113
    float noiseMode[4];    // x: 0 off, 1 overlay, 2 midtone
    float motion[4];       // c105 (CheckMotionBlur)
    float motionMode[4];   // x: 0 off, 1 prev valid, 2 first frame (no prev yet);
                           // y: 1 = the glare composite (bit 37) instead of bit 4
    float hue[4];          // c222 (CheckHueConverge); z (blend_amount) 0 = off
    float velocity[4];     // x: c122 (DoVelocity); y: 1 = bit 42 on
    float spot[4];         // bit 51: c91.x, c127.x, c127.y; w = 1 when beams are bound
    float enh[4];          // round 10, the enhanced look: x = 1 B-spline set upsample
                           // (enhFilter, plain and glare arms), y = dither amplitude
                           // (enhPrecision: 1 / 255; 0 off)
};
static_assert(sizeof(DC3CompositeUniforms) == 240, "DC3CompositeUniforms layout");

// velocity_camera.ps: last frame's clip space from this frame's NDC, and the
// target sizes (full depth w, h; velocity w, h).
struct DC3VelocityUniforms {
    float reproj[4][4];
    float size[4];
};
static_assert(sizeof(DC3VelocityUniforms) == 80, "DC3VelocityUniforms layout");

const char* kDC3Shader = R"WGSL(
struct TapUB {
    tap: array<vec4f, 7>,
};
struct DC3UB {
    xfm0: vec4f,
    xfm1: vec4f,
    xfm2: vec4f,
    bloomColor: vec4f,
    vignette: vec4f,
    chroma: vec4f,
    noiseSeeds: vec4f,
    noiseParams: vec4f,
    noiseMode: vec4f,
    motion: vec4f,
    motionMode: vec4f,
    hue: vec4f,
    velocity: vec4f,
    spot: vec4f,
    enh: vec4f,
};
struct VelUB {
    m0: vec4f,
    m1: vec4f,
    m2: vec4f,
    m3: vec4f,
    size: vec4f,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> tu: TapUB;

@group(0) @binding(6) var<uniform> cu: DC3UB;
@group(0) @binding(3) var bloom0: texture_2d<f32>;
@group(0) @binding(4) var bloom1: texture_2d<f32>;
@group(0) @binding(5) var bloom2: texture_2d<f32>;
@group(0) @binding(7) var noiseTex: texture_2d<f32>;
@group(0) @binding(8) var noiseSamp: sampler;
@group(0) @binding(9) var prevTex: texture_2d<f32>;
@group(0) @binding(10) var velTex: texture_2d<f32>;
// The velocity pass's own layout (mDC3VelBGL).
@group(0) @binding(11) var velDepth: texture_depth_multisampled_2d;
@group(0) @binding(12) var<uniform> vu: VelUB;
// Bit 51: NgSpotlightDrawer's blurred beam target (tf12) and fog density (tf5).
@group(0) @binding(13) var spotBeams: texture_2d<f32>;
@group(0) @binding(14) var spotFog: texture_2d<f32>;

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

// ---- np-gfx-post round 10: the enhanced look (LookSettings::postPrecision,
// postBloomFilter; the world grade only). kSat is true for the RGBA16Float
// families: those sets saturate after every pass, as retail's 8-bit EDRAM
// resolve does, so only the quantisation changes. The retail pipelines
// compile with the default (false).
//
// kDeadZone (the shipped 16-bit family): retail's 8-bit sets also round every
// pass's output to the nearest level, so whatever a pass leaves under half a
// level is gone -- a dim surface's own glow and the far tails of a halo. That
// is a large part of the original's look: without it the 16-bit sets bloom
// every dim surface, and the dark floor of dci rose 1.2 levels (engine doc
// §37.11, MEASURED). The quantisation's STEPS are what this family removes,
// not its dead zone, so each pass keeps the dead zone as a smooth knee:
// x * smoothstep(0.25, 0.6 levels, x), 0 below a quarter level, x above 0.6,
// C1 between (a hard cut would draw the halo's outer edge as one step).
// Unshipped [2] family (kDeadZone false): the sets linear, for the tests that
// measure the filters alone.
override kSat: bool = false;
override kDeadZone: bool = false;
fn sat_set(c: vec4f) -> vec4f {
    if (kSat) {
        let s = clamp(c, vec4f(0.0), vec4f(1.0));
        if (kDeadZone) {
            return s * smoothstep(vec4f(0.25 / 255.0), vec4f(0.6 / 255.0), s);
        }
        return s;
    }
    return c;
}

// bloom.ps as RB3RetailPost's fs_bloom_down (the exact 4x4 box), here so the
// RGBA16Float family can saturate it.
@fragment fn fs_bloom_box(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4;
    var acc = vec3f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let c = textureLoad(srcTex, min(base + vec2i(i, j), dims - 1), 0);
            acc += c.rgb * c.a;
        }
    }
    return sat_set(vec4f(acc / 16.0, 1.0));
}

// The enhanced downsample: a tent over the 8x8 texels around the target
// texel's 4x4 block, (1 - |d| / 4) per axis at d = -3.5 .. 3.5, normalised.
// Same mean as the box (energy), slightly wider (2.75 vs 1.25 texel^2 per
// axis), and a feature moving across a block boundary hands its energy over
// gradually instead of in one step (the box's shimmer).
const kTent = array<f32, 8>(0.03125, 0.09375, 0.15625, 0.21875,
                            0.21875, 0.15625, 0.09375, 0.03125);
@fragment fn fs_bloom_tent(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4 - vec2i(2);
    var acc = vec3f(0.0);
    for (var j = 0; j < 8; j++) {
        for (var i = 0; i < 8; i++) {
            let c = textureLoad(srcTex, clamp(base + vec2i(i, j), vec2i(0), dims - 1), 0);
            acc += (kTent[i] * kTent[j]) * c.rgb * c.a;
        }
    }
    return sat_set(vec4f(acc, 1.0));
}

// downsample_4x.ps (the 4x4 box), and its tent.
@fragment fn fs_down4_e(in: VOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(srcTex));
    let base = vec2i(in.pos.xy) * 4;
    var acc = vec4f(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            acc += textureLoad(srcTex, min(base + vec2i(i, j), dims - 1), 0);
        }
    }
    return sat_set(acc / 16.0);
}
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
    return sat_set(acc);
}

// blur.ps, the plain arm's 15 taps (RB3RetailPost's fs_blur: the same
// weights, centred). tap[0] = (1 / w, 1 / h, direction).
const kBlurW = array<f32, 15>(0.0159283932, 0.0270778369, 0.0424231887, 0.0612547919,
                              0.0815124959, 0.0999667868, 0.1129886061, 0.1176957935,
                              0.1129886061, 0.0999667868, 0.0815124959, 0.0612547919,
                              0.0424231887, 0.0270778369, 0.0159283932);
@fragment fn fs_blur_e(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 15; i++) {
        let o = f32(i - 7) * tu.tap[0].zw * tu.tap[0].xy;
        acc += kBlurW[i] * textureSampleLevel(srcTex, samp, in.uv + o, 0.0);
    }
    return sat_set(acc);
}

// A bloom set at uv: bilinear (retail), or the cubic B-spline in four
// bilinear taps (Sigg and Hadwiger 2005): C2-smooth, so the w/16 and w/64
// sets lose the bilinear's creases at their texel boundaries. A partition of
// unity (same energy); it widens a set by 1/6 texel^2 per axis.
fn bloom_set(t: texture_2d<f32>, uv: vec2f, bspline: bool) -> vec3f {
    if (!bspline) {
        return textureSampleLevel(t, samp, uv, 0.0).rgb;
    }
    let size = vec2f(textureDimensions(t));
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
    return g0.x * g0.y * textureSampleLevel(t, samp, p0, 0.0).rgb +
           g1.x * g0.y * textureSampleLevel(t, samp, vec2f(p1.x, p0.y), 0.0).rgb +
           g0.x * g1.y * textureSampleLevel(t, samp, vec2f(p0.x, p1.y), 0.0).rgb +
           g1.x * g1.y * textureSampleLevel(t, samp, p1, 0.0).rgb;
}

// The plain arm's three sets combined once, at set0's size, for the enhanced
// composite (postBloomFilter): set0 as it is, plus set1 and set2 B-spline
// upsampled to set0's texels, ADDED (the combine target blends One + One and
// is RGBA16Float, so the sum is not clamped, as the composite's own sum of
// the three sets is not). The composite then reads that one texture
// bilinearly, as it reads set0 in retail: one tap per pixel, not twelve. The
// coarse sets keep the B-spline's smoothness (a w/4 bilinear on top adds
// (4 px)^2 / 6 to their variance, under 1 % of it).
@fragment fn fs_comb_copy(in: VOut) -> @location(0) vec4f {
    return vec4f(textureLoad(srcTex, vec2i(in.pos.xy), 0).rgb, 1.0);
}
@fragment fn fs_comb_bspline(in: VOut) -> @location(0) vec4f {
    return vec4f(bloom_set(srcTex, in.uv, true), 0.0);
}

// A static triangular dither of +-1 LSB at the 8-bit write (two draws of
// interleaved gradient noise, Jimenez 2014): the same on every frame, so it
// adds no frame-to-frame change, and the same on all three channels.
fn dither(p: vec2f) -> f32 {
    let k = vec2f(0.06711056, 0.00583715);
    let a = fract(52.9829189 * fract(dot(p, k)));
    let b = fract(52.9829189 * fract(dot(p + vec2f(57.0, 113.0), k)));
    return a + b - 1.0;
}
// The frame write's dither (cu.enh.y; 0 = off, the colour untouched). It fades
// out within one level of 0 and 1, so black and white stay exact.
fn dithered(c: vec3f, p: vec2f) -> vec3f {
    if (cu.enh.y <= 0.0) {
        return c;
    }
    let room = clamp(min(c, vec3f(1.0) - c) * 255.0, vec3f(0.0), vec3f(1.0));
    return c + room * (dither(floor(p)) * cu.enh.y);
}

// bloom.ps with bilinear taps (tap[0..3].xy, weight 0.25 each): the scene
// weighted by its alpha, the pseudo-HDR mask.
@fragment fn fs_bloom_taps(in: VOut) -> @location(0) vec4f {
    var acc = vec3f(0.0);
    for (var i = 0; i < 4; i++) {
        let c = textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
        acc += c.rgb * c.a;
    }
    return sat_set(vec4f(acc * 0.25, 1.0));
}

// bloom_glare.ps (kBloomGlareShader, DoBloom's glare arm), decoded: ten taps
// from uv towards and through the screen centre (step 0.2 (0.5 - uv), so the
// last tap is at 1 - uv - step), each weighted by w = (1 - 4 min(|p - 0.5|^2,
// 0.25))^2 at its own position, combined as a harmonic mean:
//   out = 2 - 2 * 10 / sum(1 / (1 - w t))       (the 8-bit target saturates)
// The loop count is INFERRED from the literal 10 the sum is scaled by
// (c254.z); the loop constant itself is not in the dumped literals.
@fragment fn fs_glare(in: VOut) -> @location(0) vec4f {
    let stepUv = (1.0 - 2.0 * in.uv) * 0.1;
    var p = in.uv;
    var acc = vec3f(0.0);
    for (var i = 0; i < 10; i++) {
        let t = textureSampleLevel(srcTex, samp, p, 0.0).rgb;
        let d = p - 0.5;
        var w = 1.0 - 4.0 * min(dot(d, d), 0.25);
        w = w * w;
        acc += 1.0 / max(vec3f(1.0) - w * t, vec3f(1e-6));
        p += stepUv;
    }
    return sat_set(vec4f(2.0 - 20.0 / acc, 1.0));
}

// blur.ps with SetBloomBlurWeightsStreak's seven taps.
@fragment fn fs_streak(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 7; i++) {
        acc += tu.tap[i].z * textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
    }
    return sat_set(acc);
}

// velocity_camera.ps (RndVelocityBuffer::Draw's DrawRectDepth, decoded): the
// pixel's world position from this frame's camera and the depth (retail:
// the far-plane frustum corners scaled by the linear depth; here this frame's
// inverse view-projection, folded on the CPU with last frame's into vu.m*),
// projected by last frame's camera; v = prevUV - uv clamped to +-0.02, written
// as (v * 25 + 0.5, 1.767767 |v|, 0, 0) into the 8-bit target. One depth
// sample per output pixel: retail point-samples the full-size depth at the
// half-size pixel's centre, texel (2i + 1, 2j + 1).
@fragment fn fs_velocity(in: VOut) -> @location(0) vec4f {
    let px = vec2i(in.pos.xy);
    let src = min(px * 2 + vec2i(1), vec2i(vu.size.xy) - vec2i(1));
    let d = textureLoad(velDepth, src, 0);
    let uv = (vec2f(px) + 0.5) / vu.size.zw;
    let ndc = vec2f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let p = ndc.x * vu.m0 + ndc.y * vu.m1 + d * vu.m2 + vu.m3;
    var prevUv = uv;
    if (p.w != 0.0) {
        prevUv = vec2f(0.5 + 0.5 * p.x / p.w, 0.5 - 0.5 * p.y / p.w);
    }
    let v = clamp(prevUv - uv, vec2f(-0.02), vec2f(0.02));
    return vec4f(v * 25.0 + 0.5, 1.767767 * length(v), 0.0);
}

// postprocess.ps bit 42 (postprocess_0000040000000010, decoded): where the
// velocity target's z is at least 0.003, ten taps of the scene along the
// velocity d = (tf10.xy - 0.5) * c122, from uv - 0.023 d in steps of 0.0046 d,
// plus `centre` (the plain or chroma-split scene at uv), RGBA.
fn velocity_blur(uv: vec2f, centre: vec4f) -> vec4f {
    let vel = textureSampleLevel(velTex, samp, uv, 0.0);
    if (vel.z < 0.003) {
        return centre;
    }
    let d = (vel.xy - 0.5) * cu.velocity.x;
    let st = 0.0046 * d;
    let b = uv - 0.023 * d;
    var acc = centre * 0.19947;
    acc += (textureSampleLevel(srcTex, samp, b, 0.0) +
            textureSampleLevel(srcTex, samp, b + 9.0 * st, 0.0)) * 0.00876;
    acc += (textureSampleLevel(srcTex, samp, b + st, 0.0) +
            textureSampleLevel(srcTex, samp, b + 8.0 * st, 0.0)) * 0.02700;
    acc += (textureSampleLevel(srcTex, samp, b + 2.0 * st, 0.0) +
            textureSampleLevel(srcTex, samp, b + 7.0 * st, 0.0)) * 0.06476;
    acc += (textureSampleLevel(srcTex, samp, b + 3.0 * st, 0.0) +
            textureSampleLevel(srcTex, samp, b + 6.0 * st, 0.0)) * 0.12099;
    acc += (textureSampleLevel(srcTex, samp, b + 4.0 * st, 0.0) +
            textureSampleLevel(srcTex, samp, b + 5.0 * st, 0.0)) * 0.17603;
    return acc * 1.00549;
}

// The scene the composite grades: bit 15's chroma split (bit 43 sharpening),
// then bit 42's velocity blur. Alpha is the scene's (blurred with it).
fn scene_dc3(uv: vec2f) -> vec4f {
    let plain = textureSampleLevel(srcTex, samp, uv, 0.0);
    var scene = plain;
    if (cu.chroma.z > 0.5) {
        let o = cu.chroma.xy;
        let t0 = textureSampleLevel(srcTex, samp, uv + vec2f(-o.x, 0.0), 0.0).rgb;
        let t1 = textureSampleLevel(srcTex, samp, uv + vec2f(0.5 * o.x, 0.866 * o.y), 0.0).rgb;
        let t2 = textureSampleLevel(srcTex, samp, uv + vec2f(0.5 * o.x, -0.866 * o.y), 0.0).rgb;
        if (cu.chroma.w > 0.5) {
            scene = vec4f(4.0 * plain.rgb - t0 - t1 - t2, plain.a);
        } else {
            scene = vec4f(t0.r, t1.g, t2.b, plain.a);
        }
    }
    if (cu.velocity.y > 0.5) {
        scene = velocity_blur(uv, scene);
    }
    return scene;
}

// postprocess.ps up to the motion blur: the graded colour, UNCLAMPED (the
// shader's single saturate is on the final write).
fn grade_dc3(uv: vec2f, scene: vec3f) -> vec3f {
    // Bit 37 (DoBloom's glare arm, unk28): the glare set ADDED at half the
    // bloom colour, c = scene + 0.5 * c6 * tf7 (postprocess_0000002001200000).
    // Bit 4: screen blend with the three bloom sets (unclamped).
    var c = scene;
    let bs = cu.enh.x > 0.5;
    if (cu.motionMode.y > 0.5) {
        c = scene + 0.5 * cu.bloomColor.rgb * bloom_set(bloom0, uv, bs);
    } else if (cu.bloomColor.w > 0.5) {
        let b = bloom_set(bloom0, uv, bs) + bloom_set(bloom1, uv, bs) + bloom_set(bloom2, uv, bs);
        c = 1.0 - (1.0 - b * cu.bloomColor.rgb) * (1.0 - c);
    }

    // Bit 51 (postprocess_0008000000000000; the same place in the full
    // 0008001001208014 chain): NgSpotlightDrawer's blurred beams, weighted by
    // the fog density, ADDED after the bloom blend and before the noise, the
    // matrix and the vignette, unclamped. Alpha (the bloom mask) is untouched.
    if (cu.spot.w > 0.5) {
        let beam = textureSampleLevel(spotBeams, samp, uv, 0.0).rgb;
        let fog = textureSampleLevel(spotFog, samp, uv, 0.0).r;
        c += beam * (fog * cu.spot.z + cu.spot.y) * cu.spot.x;
    }

    // Bits 2 / 47: the noise map, overlay-blended into the screen result S
    // (no clamp on S in the microcode). n = map(base uv) * map(top uv).
    if (cu.noiseMode.x > 0.5) {
        let baseUv = (uv + cu.noiseSeeds.xy) * cu.noiseParams.xy;
        let topUv = (uv + cu.noiseSeeds.zw) * cu.noiseParams.xy * cu.noiseParams.z;
        let n = textureSampleLevel(noiseTex, noiseSamp, baseUv, 0.0).rgb *
                textureSampleLevel(noiseTex, noiseSamp, topUv, 0.0).rgb;
        let s = c;
        let intensity = cu.noiseParams.w;
        if (cu.noiseMode.x < 1.5) {
            let lo = 2.0 * n * s;
            let hi = 1.0 - 2.0 * (1.0 - n) * (1.0 - s);
            c = mix(s, select(hi, lo, s < vec3f(0.5)), intensity);
        } else {
            let r = sqrt(n);
            let luma = dot(s, vec3f(0.3, 0.59, 0.11));
            let lo = 2.0 * r * s;
            let hi = 1.0 - 2.0 * (1.0 - r) * (1.0 - s);
            let k = intensity * 6.75 * luma * (1.0 - luma) * (1.0 - luma);
            c = s + k * (select(hi, lo, luma <= 0.5) - s);
        }
    }

    // Bit 21: dp4 against the colour matrix (saturating unless the vignette
    // follows, which saturates instead).
    let c4 = vec4f(c, 1.0);
    c = vec3f(dot(cu.xfm0, c4), dot(cu.xfm1, c4), dot(cu.xfm2, c4));

    // Bit 36: the vignette (c123), after the matrix.
    if (cu.vignette.w != 0.0) {
        let d = uv - 0.5;
        let w0 = clamp((dot(d, d) + cu.vignette.w) * 3.724 - 4.655, 0.0, 1.0);
        c = c + (w0 * w0) * (cu.vignette.rgb - c);
    }
    return c;
}

// Bit 62, NgPostProc::CheckHueConverge (postprocess_4000000001200010 and its
// siblings, decoded and checked against a Xenos interpreter of the listing):
// the colour (after motion blur, unclamped) to HSV, the hue moved to
// frac(T + 2 dist (1 - focus)), dist = the circular distance of H from T =
// c222.x, V raised to c222.w unless it is 1, back to RGB, mixed in by c222.z.
fn hue_converge(c: vec3f) -> vec3f {
    let mx = max(max(c.r, c.g), c.b);
    let mn = min(min(c.r, c.g), c.b);
    let delta = mx - mn;
    var h = 0.0;
    var s = 0.0;
    if (delta != 0.0) {
        s = delta / mx;
        let d = ((vec3f(mx) - c) * (1.0 / 6.0) + vec3f(delta * 0.5)) / delta;
        if (c.r == mx) {
            h = d.b - d.g;
        } else if (c.g == mx) {
            h = 1.0 / 3.0 + d.r - d.b;
        } else if (c.b == mx) {
            h = 2.0 / 3.0 + d.g - d.r;
        }
        if (h < 0.0) { h = h + 1.0; }
        if (h > 1.0) { h = h - 1.0; }
    }
    let dd = abs(h - cu.hue.x);
    let dist = select(dd, 1.0 - dd, dd > 0.5);
    var hn = cu.hue.x + 2.0 * dist * (1.0 - cu.hue.y);
    if (hn > 1.0) { hn = hn - 1.0; }
    if (hn < 0.0) { hn = hn + 1.0; }
    var v = mx;
    if (cu.hue.w != 1.0) {
        v = select(exp2(cu.hue.w * log2(abs(mx))), 0.0, mx == 0.0);
    }
    var rgb = vec3f(v);
    if (s != 0.0) {
        let h6 = hn * 6.0;
        let i = floor(h6);
        let f = h6 - i;
        let p = v * (1.0 - s);
        let q = v * (1.0 - f * s);
        let t = v * (1.0 - (1.0 - f) * s);
        if (i == 0.0) {
            rgb = vec3f(v, t, p);
        } else if (i == 1.0) {
            rgb = vec3f(q, v, p);
        } else if (i == 2.0) {
            rgb = vec3f(p, v, t);
        } else if (i == 3.0) {
            rgb = vec3f(p, q, v);
        } else if (i == 4.0) {
            rgb = vec3f(t, p, v);
        } else {
            rgb = vec3f(v, p, q);
        }
    }
    return c + (rgb - c) * cu.hue.z;
}

@fragment fn fs_composite_dc3(in: VOut) -> @location(0) vec4f {
    // Inspection views: what bit 42 changed in THIS frame (|blurred - scene|
    // x 8, black where it drew nothing: one frame, so a before/after with no
    // timing between the two), the velocity target (tf10 as stored; (0.5,
    // 0.5, 0) where none ran), the bloom mask (scene alpha) and the bloom term.
    if (cu.bloomColor.w > 4.5) {
        let plain = textureSampleLevel(srcTex, samp, in.uv, 0.0);
        if (cu.velocity.y > 0.5) {
            let d = abs(velocity_blur(in.uv, plain).rgb - plain.rgb) * 8.0;
            return vec4f(clamp(d, vec3f(0.0), vec3f(1.0)), 1.0);
        }
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }
    if (cu.bloomColor.w > 3.5) {
        if (cu.velocity.y > 0.5) {
            return vec4f(textureSampleLevel(velTex, samp, in.uv, 0.0).rgb, 1.0);
        }
        return vec4f(0.5, 0.5, 0.0, 1.0);
    }
    if (cu.bloomColor.w > 2.5) {
        let bs = cu.enh.x > 0.5;
        let b = bloom_set(bloom0, in.uv, bs) + bloom_set(bloom1, in.uv, bs) + bloom_set(bloom2, in.uv, bs);
        return vec4f(clamp(b * cu.bloomColor.rgb, vec3f(0.0), vec3f(1.0)), 1.0);
    }
    if (cu.bloomColor.w > 1.5) {
        let a = textureSampleLevel(srcTex, samp, in.uv, 0.0).a;
        return vec4f(a, a, a, 1.0);
    }
    var c = grade_dc3(in.uv, scene_dc3(in.uv).rgb);
    if (cu.hue.z > 0.0) {
        c = hue_converge(c);
    }
    return vec4f(dithered(clamp(c, vec3f(0.0), vec3f(1.0)), in.pos.xy), 1.0);
}

// Bit 24, NgPostProc::CheckMotionBlur (postprocess_0000000001000010 and its
// siblings, decoded): tf14 is the previous frame's post-process output. Its
// alpha is the previous output's, and this output's alpha is what the next
// frame reads. Xenos rcp(0) = inf with 0 * inf = 0 makes r 0 when prev.a is 0.
struct MBOut {
    @location(0) frame: vec4f,
    @location(1) next: vec4f,
};
@fragment fn fs_composite_dc3_mb(in: VOut) -> MBOut {
    let scene = scene_dc3(in.uv);
    let c = grade_dc3(in.uv, scene.rgb);
    let sa = clamp(scene.a, 0.0, 1.0);
    var o: vec3f;
    var a: f32;
    if (cu.motionMode.x > 1.5) {
        o = c;
        a = sa;
    } else {
        let prev = textureSampleLevel(prevTex, samp, in.uv, 0.0);
        var r = 0.0;
        if (prev.a > 0.0) {
            r = clamp(cu.motion.w * (prev.a - sa) / prev.a, 0.0, 1.0);
        }
        let k = max(vec3f(r), cu.motion.xyz);
        o = c + (prev.rgb - c) * k;
        a = clamp(sa + r * (prev.a - sa), 0.0, 1.0);
    }
    // Bit 62 follows the motion blur, so the next frame's tf14 carries it.
    if (cu.hue.z > 0.0) {
        o = hue_converge(o);
    }
    o = clamp(o, vec3f(0.0), vec3f(1.0));
    var out: MBOut;
    out.frame = vec4f(dithered(o, in.pos.xy), 1.0);
    out.next = vec4f(o, a);
    return out;
}
)WGSL";

}  // namespace

void RB3RetailPost::EnsureDC3Pipelines(GpuDevice& gpu) {
    // DC3's bloom sets are 8-bit (NgPostProc::BloomTextureSet: TheRnd.Bpp(),
    // A8R8G8B8), so each pass clamps at 1. Rebuild RB3's shared pass
    // pipelines and sets in that format once.
    if (mBloomFormat != wgpu::TextureFormat::RGBA8Unorm) {
        mBloomFormat = wgpu::TextureFormat::RGBA8Unorm;
        mReady = false;
        mDC3Ready = false;
        for (int s = 0; s < kSets; s++) {
            mW[s] = mH[s] = 0;
            for (int k = 0; k < 2; k++) {
                Forget(mView[s][k]);
                mTex[s][k] = nullptr;
                mView[s][k] = nullptr;
            }
        }
    }
    EnsurePipelines(gpu);
    if (mDC3Ready && mDC3FrameFormat == gpu.SurfaceFormat()) return;
    auto& dev = gpu.Device();
    mDC3FrameFormat = gpu.SurfaceFormat();
    for (DC3Family& f : mDC3Fam) f = DC3Family{};   // built on mDC3Shader / mDC3PassPL

    wgpu::ShaderSourceWGSL src;
    src.code = kDC3Shader;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    smDesc.label = "DC3RetailPost";
    mDC3Shader = dev.CreateShaderModule(&smDesc);

    if (!mNoiseSampler) {
        // NgPostProc::CheckNoise: SetTextureFilter(0xd, 1) (linear),
        // SetTextureClamp(0xd, 0) (wrap); the map has no mips used.
        SamplerDesc nd{};
        nd.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        mNoiseSampler = gpu.GetSampler(nd);
    }

    auto texEntry = [](uint32_t b) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.texture.sampleType = wgpu::TextureSampleType::Float;
        e.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        return e;
    };
    auto sampEntry = [](uint32_t b) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.sampler.type = wgpu::SamplerBindingType::Filtering;
        return e;
    };
    auto uniEntry = [](uint32_t b, uint64_t size) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = b;
        e.visibility = wgpu::ShaderStage::Fragment;
        e.buffer.type = wgpu::BufferBindingType::Uniform;
        e.buffer.minBindingSize = size;
        e.buffer.hasDynamicOffset = true;   // the uniform slot (NextSlot)
        return e;
    };
    {
        wgpu::BindGroupLayoutEntry e[3] = {texEntry(0), sampEntry(1), uniEntry(2, sizeof(TapUniforms))};
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 3;
        d.entries = e;
        mDC3PassBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::BindGroupLayoutEntry e[12] = {texEntry(0), sampEntry(1), uniEntry(6, sizeof(DC3CompositeUniforms)),
                                            texEntry(3), texEntry(4), texEntry(5), texEntry(7), sampEntry(8),
                                            texEntry(9), texEntry(10), texEntry(13), texEntry(14)};
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 12;
        d.entries = e;
        mDC3CompositeBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        // The velocity pass: the multisampled world depth (textureLoad, sample
        // 0) and its uniform.
        wgpu::BindGroupLayoutEntry e[2] = {};
        e[0].binding = 11;
        e[0].visibility = wgpu::ShaderStage::Fragment;
        e[0].texture.sampleType = wgpu::TextureSampleType::Depth;
        e[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[0].texture.multisampled = true;
        e[1] = uniEntry(12, sizeof(DC3VelocityUniforms));
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 2;
        d.entries = e;
        mDC3VelBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 1;
        d.bindGroupLayouts = &mDC3PassBGL;
        mDC3PassPL = dev.CreatePipelineLayout(&d);
        d.bindGroupLayouts = &mDC3CompositeBGL;
        mDC3CompositePL = dev.CreatePipelineLayout(&d);
        d.bindGroupLayouts = &mDC3VelBGL;
        mDC3VelPL = dev.CreatePipelineLayout(&d);
    }
    auto make = [&](const char* fs, const wgpu::PipelineLayout& pl, wgpu::TextureFormat fmt) {
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mDC3Shader;
        frag.entryPoint = fs;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = fs;
        pd.layout = pl;
        pd.vertex.module = mDC3Shader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return CreateCountedRenderPipeline(dev, &pd);
    };
    mDC3BloomTapsPipe = make("fs_bloom_taps", mDC3PassPL, mBloomFormat);
    mDC3StreakPipe = make("fs_streak", mDC3PassPL, mBloomFormat);
    mDC3GlarePipe = make("fs_glare", mDC3PassPL, mBloomFormat);
    mDC3CompositePipe = make("fs_composite_dc3", mDC3CompositePL, mDC3FrameFormat);
    // RndVelocityBuffer::AllocateData: SetBitmap(w / 2, h / 2, TheRnd.Bpp() =
    // 32, kRendered): A8R8G8B8.
    mDC3VelPipe = make("fs_velocity", mDC3VelPL, wgpu::TextureFormat::RGBA8Unorm);
    {
        // The motion-blur composite: the frame, and the feedback target the
        // next frame reads as tf14 (A8R8G8B8, as mPostProcessBuffer).
        wgpu::ColorTargetState ct[2] = {};
        ct[0].format = mDC3FrameFormat;
        ct[0].writeMask = wgpu::ColorWriteMask::All;
        ct[1].format = wgpu::TextureFormat::RGBA8Unorm;
        ct[1].writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mDC3Shader;
        frag.entryPoint = "fs_composite_dc3_mb";
        frag.targetCount = 2;
        frag.targets = ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = "fs_composite_dc3_mb";
        pd.layout = mDC3CompositePL;
        pd.vertex.module = mDC3Shader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        mDC3CompositeMBPipe = CreateCountedRenderPipeline(dev, &pd);
    }
    mDC3Ready = true;
}

void RB3RetailPost::PrewarmDC3Enhanced(bool precision, bool filter, GpuDevice& gpu) {
    if (!precision && !filter) return;
    EnsureDC3Pipelines(gpu);
    EnsureDC3Family(precision ? 1 : 0, gpu);   // RunDC3Bloom's `fam` (enhLinear is tests only)
}

// np-gfx-post round 10: the enhanced look's bloom passes (RetailPostParams::
// enhPrecision / enhFilter), one family per set format. [1] (RGBA16Float)
// compiles with kSat, so each pass saturates where the 8-bit sets would.
void RB3RetailPost::EnsureDC3Family(int idx, GpuDevice& gpu) {
    DC3Family& f = mDC3Fam[idx];
    if (f.blur) return;
    const wgpu::TextureFormat fmt = idx ? wgpu::TextureFormat::RGBA16Float : wgpu::TextureFormat::RGBA8Unorm;
    wgpu::ConstantEntry sat{};
    sat.key = "kSat";
    sat.value = idx ? 1.0 : 0.0;
    wgpu::ConstantEntry kc[2] = {sat, {}};
    kc[1].key = "kDeadZone";
    kc[1].value = idx == 1 ? 1.0 : 0.0;
    auto make = [&](const char* fs, const char* label) {
        wgpu::ColorTargetState ct{};
        ct.format = fmt;
        ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = mDC3Shader;
        frag.entryPoint = fs;
        frag.constantCount = 2;
        frag.constants = kc;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = label;
        pd.layout = mDC3PassPL;
        pd.vertex.module = mDC3Shader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return CreateCountedRenderPipeline(gpu.Device(), &pd);
    };
    f.bloomBox = make("fs_bloom_box", "DC3LookBloomBox");
    f.bloomTent = make("fs_bloom_tent", "DC3LookBloomTent");
    f.bloomTaps = make("fs_bloom_taps", "DC3LookBloomTaps");
    f.down4 = make("fs_down4_e", "DC3LookDown4");
    f.downTent = make("fs_down_tent", "DC3LookDownTent");
    f.streak = make("fs_streak", "DC3LookStreak");
    f.glare = make("fs_glare", "DC3LookGlare");
    f.blur = make("fs_blur_e", "DC3LookBlur");
    auto makeComb = [&](const char* fs, const char* label, bool add) {
        wgpu::BlendState blend{};
        blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::One};
        blend.alpha = blend.color;
        wgpu::ColorTargetState ct{};
        ct.format = wgpu::TextureFormat::RGBA16Float;
        ct.writeMask = wgpu::ColorWriteMask::All;
        if (add) ct.blend = &blend;
        wgpu::FragmentState frag{};
        frag.module = mDC3Shader;
        frag.entryPoint = fs;
        frag.targetCount = 1;
        frag.targets = &ct;
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = label;
        pd.layout = mDC3PassPL;
        pd.vertex.module = mDC3Shader;
        pd.vertex.entryPoint = "vs_full";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        return CreateCountedRenderPipeline(gpu.Device(), &pd);
    };
    f.combCopy = makeComb("fs_comb_copy", "DC3LookCombCopy", false);
    f.combAdd = makeComb("fs_comb_bspline", "DC3LookCombAdd", true);
}

void RB3RetailPost::EnsureHiTextures(GpuDevice& gpu) {
    for (int s = 0; s < kSets; s++) {
        if (mHW[s] == mW[s] && mHH[s] == mH[s] && mHTex[s][0]) continue;
        for (int k = 0; k < 2; k++) {
            Forget(mHView[s][k]);
            wgpu::TextureDescriptor td{};
            td.label = "DC3LookBloom";
            td.size = {(uint32_t)mW[s], (uint32_t)mH[s], 1};
            td.format = wgpu::TextureFormat::RGBA16Float;
            td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            mHTex[s][k] = gpu.Device().CreateTexture(&td);
            mHView[s][k] = mHTex[s][k].CreateView();
        }
        mHW[s] = mW[s];
        mHH[s] = mH[s];
    }
}

void RB3RetailPost::EnsureCombTexture(GpuDevice& gpu) {
    if (mCombTex && mCombW == mW[0] && mCombH == mH[0]) return;
    Forget(mCombView);
    wgpu::TextureDescriptor td{};
    td.label = "DC3LookBloomComb";
    td.size = {(uint32_t)mW[0], (uint32_t)mH[0], 1};
    td.format = wgpu::TextureFormat::RGBA16Float;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    mCombTex = gpu.Device().CreateTexture(&td);
    mCombView = mCombTex.CreateView();
    mCombW = mW[0];
    mCombH = mH[0];
}

void RB3RetailPost::PassDC3(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                            const wgpu::TextureView& dst, const wgpu::TextureView& src,
                            const float* uni, size_t uniBytes, GpuDevice& gpu, const char* label,
                            bool load) {
    const uint32_t off = NextSlot();
    gpu.Queue().WriteBuffer(mUniforms, off, uni, uniBytes);

    wgpu::BindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].textureView = src;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 2;
    e[2].buffer = mUniforms;
    e[2].size = uniBytes;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3PassBGL;
    bgd.entryCount = 3;
    bgd.entries = e;
    wgpu::BindGroup bg = Group(gpu, bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = label;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    MILO_GPU_TIME_PASS(rp);
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg, 1, &off);
    pass.Draw(3);
    pass.End();
}

// SetBloomBlurWeightsStreak (dc3-decomp rndobj/Utl.cpp), as uv offsets.
static constexpr float kStreakCentreOffset = 0.5f;
static TapUniforms StreakTaps(bool horizontal, float w, float h, float attenuation, int pass,
                              float angleDeg, float yRatio) {
    float weights[7], offsets[7];
    const float scale = std::pow(4.0f, (float)pass);
    const float atten = std::pow(attenuation, scale);
    // Retail's offsets are 0.5 +- i * scale (initOffset = 0.5f in Utl.cpp).
    // DxRnd::DrawRect draws with D3DDevice_SetRenderState_HalfPixelOffset(1),
    // so a pixel centre already samples its texel centre, exactly as here: the
    // 0.5 is NOT a convention fix but a real half-texel step along the
    // streak's axis on every pass (MEASURED against the original under
    // Xenia on frozen frames, engine doc §37.10).
    weights[3] = 1.0f / 3.0f;
    offsets[3] = kStreakCentreOffset;
    float curWeight = atten;
    for (int i = 1; i <= 3; i++) {
        weights[3 - i] = weights[3 + i] = curWeight * (1.0f / 3.0f);
        offsets[3 - i] = kStreakCentreOffset - (float)i * scale;
        offsets[3 + i] = kStreakCentreOffset + (float)i * scale;
        curWeight *= atten;
    }
    const float a = angleDeg * 0.017453292f;
    const float s = std::sin(a), c = std::cos(a);
    TapUniforms t{};
    for (int i = 0; i < 7; i++) {
        if (horizontal) {
            const float off = offsets[i] / w;
            t.tap[i][0] = off * c * yRatio;
            t.tap[i][1] = off * s;
        } else {
            const float off = offsets[i] / h;
            t.tap[i][0] = -(off * s * yRatio);
            t.tap[i][1] = off * c;
        }
        t.tap[i][2] = weights[i];
    }
    return t;
}

void RB3RetailPost::RunDC3Bloom(wgpu::CommandEncoder& encoder, const wgpu::TextureView& bloomSrc,
                                int sceneW, int sceneH, const RetailPostParams& p, GpuDevice& gpu) {
    EnsureTextures(sceneW, sceneH, gpu);
    // The enhanced look (round 10) runs the same passes in the same order on
    // its own family and, with enhPrecision, its own RGBA16Float sets; both
    // off, every pass below is retail's.
    const bool enh = p.enhPrecision || p.enhFilter;
    DC3Family* f = nullptr;
    if (enh) {
        const int fam = !p.enhPrecision ? 0 : p.enhLinear ? 2 : 1;
        EnsureDC3Family(fam, gpu);
        f = &mDC3Fam[fam];
        if (p.enhPrecision) EnsureHiTextures(gpu);
    }
    wgpu::TextureView (*V)[2] = p.enhPrecision ? mHView : mView;
    struct PU { float srcTexel[2], dir[2], lumaScale, pad[3]; };
    // bloom.ps: retail's exact 4x4 box, or the tent.
    auto bloomPass = [&](const wgpu::TextureView& dst) {
        if (!enh) {
            PU u{};
            u.srcTexel[0] = 1.0f / sceneW;
            u.srcTexel[1] = 1.0f / sceneH;
            u.lumaScale = 1.0f;
            Pass(encoder, mBloomDownPipe, dst, bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
            return;
        }
        TapUniforms t{};
        PassDC3(encoder, p.enhFilter ? f->bloomTent : f->bloomBox, dst, bloomSrc, &t.tap[0][0], sizeof(t),
                gpu, "DC3LookBloom");
    };
    // downsample_4x from set s - 1 into set s (`tent`: the tent, when enhFilter).
    auto downPass = [&](const wgpu::TextureView& dst, const wgpu::TextureView& src, int s, bool tent) {
        if (!enh) {
            PU d{};
            d.srcTexel[0] = 1.0f / mW[s - 1];
            d.srcTexel[1] = 1.0f / mH[s - 1];
            Pass(encoder, mDown4Pipe, dst, src, &d.srcTexel[0], sizeof(d), gpu);
            return;
        }
        TapUniforms t{};
        PassDC3(encoder, tent && p.enhFilter ? f->downTent : f->down4, dst, src, &t.tap[0][0], sizeof(t), gpu,
                "DC3LookBloom");
    };
    // blur.ps, the plain 15 taps on set s.
    auto blurPass = [&](const wgpu::TextureView& dst, const wgpu::TextureView& src, int s, bool horizontal) {
        if (!enh) {
            PU b{};
            b.srcTexel[0] = 1.0f / mW[s];
            b.srcTexel[1] = 1.0f / mH[s];
            b.dir[horizontal ? 0 : 1] = 1.0f;
            Pass(encoder, mBlurPipe, dst, src, &b.srcTexel[0], sizeof(b), gpu);
            return;
        }
        TapUniforms t{};
        t.tap[0][0] = 1.0f / mW[s];
        t.tap[0][1] = 1.0f / mH[s];
        t.tap[0][horizontal ? 2 : 3] = 1.0f;
        PassDC3(encoder, f->blur, dst, src, &t.tap[0][0], sizeof(t), gpu, "DC3LookBloom");
    };
    // The DC3 module's own passes (bloom taps, streak, glare).
    auto tapPass = [&](const wgpu::RenderPipeline& retail, const wgpu::RenderPipeline DC3Family::*fam,
                       const wgpu::TextureView& dst, const wgpu::TextureView& src, const TapUniforms& t) {
        if (!enh) {
            PassDC3(encoder, retail, dst, src, &t.tap[0][0], sizeof(t), gpu);
            return;
        }
        PassDC3(encoder, f->*fam, dst, src, &t.tap[0][0], sizeof(t), gpu, "DC3LookBloom");
    };
    if (p.glare) {
        // DoBloom's glare arm (mBloomGlare, not the hi-res screen): bloom.ps
        // into set0, the plain 15-tap blur across and down, then one
        // bloom_glare.ps pass; set0 alone feeds the composite (tf7), whose
        // bit 37 adds it. Retail ping-pongs on one texture (tex[1] == tex[0]);
        // ordered here to end in [0][0].
        bloomPass(V[0][1]);
        blurPass(V[0][0], V[0][1], 0, true);
        blurPass(V[0][1], V[0][0], 0, false);
        TapUniforms t{};
        tapPass(mDC3GlarePipe, &DC3Family::glare, V[0][0], V[0][1], t);
        return;
    }
    if (!p.streak) {
        // The plain arm is RB3's: bloom.ps into set0, then 15-tap blurs and
        // downsample_4x into set1 and set2 (identical in DC3's DoBloom).
        bloomPass(V[0][0]);
        for (int s = 0; s < kSets; s++) {
            if (s > 0) downPass(V[s][0], V[s - 1][0], s, true);
            blurPass(V[s][1], V[s][0], s, true);
            blurPass(V[s][0], V[s][1], s, false);
        }
        if (enh && p.enhFilter) {
            // The combine (fs_comb_*): the composite reads one texture.
            EnsureCombTexture(gpu);
            TapUniforms t{};
            PassDC3(encoder, f->combCopy, mCombView, V[0][0], &t.tap[0][0], sizeof(t), gpu, "DC3LookBloom");
            for (int s = 1; s < kSets; s++)
                PassDC3(encoder, f->combAdd, mCombView, V[s][0], &t.tap[0][0], sizeof(t), gpu, "DC3LookBloom",
                        true);
        }
        return;
    }
    // The streak arm. set0: bloom.ps into w/4 (exact 4x4 box), three streak
    // passes along the rotated horizontal, ping-ponging to end in [0][0].
    bloomPass(V[0][1]);
    if (enh && p.enhFilter) {
        // enhFilter: set1 is the 4x4 box of set0's bloom output, before its
        // streak passes, so every scene texel of the 16x16 block counts.
        downPass(V[1][1], V[0][1], 1, false);
    }
    const wgpu::TextureView* ping[3][2] = {{&V[0][1], &V[0][0]},
                                           {&V[0][0], &V[0][1]},
                                           {&V[0][1], &V[0][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(true, (float)mW[0], (float)mH[0], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        tapPass(mDC3StreakPipe, &DC3Family::streak, *ping[pass][1], *ping[pass][0], t);
    }
    // set1: bloom.ps straight from the scene into w/16, then three passes
    // along the rotated vertical.
    if (!(enh && p.enhFilter)) {
        TapUniforms t{};
        const float tx = 2.0f / sceneW, ty = 2.0f / sceneH;
        const float o[4][2] = {{-tx, -ty}, {tx, ty}, {tx, -ty}, {-tx, ty}};
        for (int i = 0; i < 4; i++) {
            t.tap[i][0] = o[i][0];
            t.tap[i][1] = o[i][1];
            t.tap[i][2] = 0.25f;
        }
        tapPass(mDC3BloomTapsPipe, &DC3Family::bloomTaps, V[1][1], bloomSrc, t);
    }
    const wgpu::TextureView* ping1[3][2] = {{&V[1][1], &V[1][0]},
                                            {&V[1][0], &V[1][1]},
                                            {&V[1][1], &V[1][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(false, (float)mW[1], (float)mH[1], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        tapPass(mDC3StreakPipe, &DC3Family::streak, *ping1[pass][1], *ping1[pass][0], t);
    }
}

void RB3RetailPost::RunDC3Composite(wgpu::CommandEncoder& encoder, const wgpu::TextureView& sceneView,
                                    int sceneW, int sceneH, const wgpu::TextureView& frameView,
                                    const RetailPostParams& p, GpuDevice& gpu) {
    DC3CompositeUniforms cu{};
    std::memcpy(cu.xfm, p.xfm, sizeof(cu.xfm));
    cu.bloomColor[0] = p.bloomColor[0];
    cu.bloomColor[1] = p.bloomColor[1];
    cu.bloomColor[2] = p.bloomColor[2];
    cu.bloomColor[3] = p.debugView == 1 ? 2.0f : p.debugView == 2 ? 3.0f : p.debugView == 4 ? 4.0f
                     : p.debugView == 5 ? 5.0f
                     : p.bloom ? 1.0f : 0.0f;
    cu.vignette[0] = p.vignetteColor[0];
    cu.vignette[1] = p.vignetteColor[1];
    cu.vignette[2] = p.vignetteColor[2];
    cu.vignette[3] = p.vignetteIntensity;
    // CheckChromaticAberration: c121 = offset / (TheRnd.Width(), Height()) --
    // output pixels, so a render-scaled scene divides by its output size.
    if (p.chromaticOffset != 0.0f) {
        const float rs = p.renderScale > 0.0f ? p.renderScale : 1.0f;
        cu.chroma[0] = p.chromaticOffset * rs / (float)sceneW;
        cu.chroma[1] = p.chromaticOffset * rs / (float)sceneH;
        cu.chroma[2] = 1.0f;
        cu.chroma[3] = p.chromaticSharpen;
    }
    const bool haveNoise = (bool)p.noiseMap;
    if (haveNoise) {
        std::memcpy(cu.noiseSeeds, p.noiseSeeds, sizeof(cu.noiseSeeds));
        std::memcpy(cu.noiseParams, p.noiseParams, sizeof(cu.noiseParams));
        cu.noiseMode[0] = p.noiseMidtone > 0.5f ? 2.0f : 1.0f;
    }
    // CheckMotionBlur. Retail resolves mPostProcessBuffer on EVERY frame; this
    // keeps the feedback target only while motion blur is on, so the first
    // frame after it turns on (or after a resize) has no previous output and
    // passes the graded colour through (one frame, then retail's feedback).
    const uint64_t run = ++mRunIndex;
    const bool mb = p.motionBlur && p.debugView == 0;
    wgpu::TextureView prevRead = mBlackView, prevWrite;
    if (mb) {
        if (!mPrevTex[0] || mPrevW != sceneW || mPrevH != sceneH) {
            for (int i = 0; i < 2; i++) {
                Forget(mPrevView[i]);
                wgpu::TextureDescriptor td{};
                td.label = "DC3PostProcessBuffer";
                td.size = {(uint32_t)sceneW, (uint32_t)sceneH, 1};
                td.format = wgpu::TextureFormat::RGBA8Unorm;
                td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
                td.mipLevelCount = 1;
                mPrevTex[i] = gpu.CreateTexture(td);
                mPrevView[i] = mPrevTex[i].CreateView();
            }
            mPrevW = sceneW;
            mPrevH = sceneH;
            mPrevRun = ~0ull;
        }
        const bool valid = mPrevRun == run - 1;
        std::memcpy(cu.motion, p.motionBlurParams, sizeof(cu.motion));
        cu.motionMode[0] = valid ? 1.0f : 2.0f;
        if (valid) prevRead = mPrevView[mPrevCur];
        mPrevCur ^= 1;
        prevWrite = mPrevView[mPrevCur];
        mPrevRun = run;
    }
    // Bit 37 (the glare composite) replaces bit 4's screen blend.
    cu.motionMode[1] = (p.glare && p.bloom && p.debugView == 0) ? 1.0f : 0.0f;
    if (p.hueConverge && p.debugView == 0) {
        std::memcpy(cu.hue, p.hueParams, sizeof(cu.hue));
    }
    // Bit 42: only on a frame whose velocity pass ran (RunDC3Velocity).
    const bool vel = p.velocity && (p.debugView == 0 || p.debugView >= 4) && mVelView && mVelRun == run;
    if (vel) {
        cu.velocity[0] = p.velocityScale;
        cu.velocity[1] = 1.0f;
    }
    // Bit 51: this frame's spotlight beams (WgpuRnd::RunDC3SpotBeams).
    const bool haveSpot = (bool)p.spotBeams && p.debugView == 0;
    if (haveSpot) {
        cu.spot[0] = p.spotScale;
        cu.spot[1] = p.spotBase;
        cu.spot[2] = p.spotSmoke;
        cu.spot[3] = 1.0f;
    }
    // Round 10, the enhanced look: the B-spline upsample (not the streak
    // arm's sets, whose thin perpendicular extent it would widen) and the
    // frame write's dither. The plain arm's sets come combined (RunDC3Bloom,
    // fs_comb_*), already B-spline upsampled, so only the glare arm's set0
    // is B-splined here.
    const bool comb = p.enhFilter && !p.streak && !p.glare && mCombView;
    if (p.enhFilter && p.glare) cu.enh[0] = 1.0f;
    if (p.enhPrecision) cu.enh[1] = 1.0f / 255.0f;
    const uint32_t off = NextSlot();
    gpu.Queue().WriteBuffer(mUniforms, off, &cu, sizeof(cu));

    // The plain arm fills all three sets; the streak arm two (tf15 black).
    const wgpu::TextureView (*V)[2] = p.enhPrecision ? mHView : mView;
    const bool haveBloom = (p.bloom || p.debugView == 2) && V[0][0];
    const int setsUsed = p.glare ? 1 : p.streak ? 2 : 3;
    wgpu::BindGroupEntry e[12] = {};
    e[0].binding = 0;
    e[0].textureView = sceneView;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 6;
    e[2].buffer = mUniforms;
    e[2].size = sizeof(cu);
    for (int s = 0; s < kSets; s++) {
        e[3 + s].binding = 3 + s;
        e[3 + s].textureView = !(haveBloom && s < setsUsed) ? mBlackView
                             : comb ? (s == 0 ? mCombView : mBlackView)
                                    : V[s][0];
    }
    e[6].binding = 7;
    e[6].textureView = haveNoise ? p.noiseMap : mBlackView;
    e[7].binding = 8;
    e[7].sampler = mNoiseSampler;
    e[8].binding = 9;
    e[8].textureView = prevRead;
    e[9].binding = 10;
    e[9].textureView = vel ? mVelView : mBlackView;
    e[10].binding = 13;
    e[10].textureView = haveSpot ? p.spotBeams : mBlackView;
    e[11].binding = 14;
    e[11].textureView = haveSpot && p.spotFog ? p.spotFog : mBlackView;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3CompositeBGL;
    bgd.entryCount = 12;
    bgd.entries = e;
    // Motion blur ping-pongs the feedback target, so two groups alternate.
    wgpu::BindGroup bg = Group(gpu, bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = frameView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassColorAttachment cas[2] = {ca, ca};
    cas[1].view = prevWrite;
    cas[1].loadOp = wgpu::LoadOp::Clear;
    cas[1].clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3RetailPostComposite";
    rp.colorAttachmentCount = mb ? 2 : 1;
    rp.colorAttachments = cas;
    MILO_GPU_TIME_PASS(rp);
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mb ? mDC3CompositeMBPipe : mDC3CompositePipe);
    pass.SetBindGroup(0, bg, 1, &off);
    pass.Draw(3);
    pass.End();
}

// RndVelocityBuffer::Draw's camera pass (velocity_camera.ps) into tf10.
bool RB3RetailPost::RunDC3Velocity(wgpu::CommandEncoder& encoder, int sceneW, int sceneH,
                                   const RetailPostParams& p, GpuDevice& gpu) {
    if (!p.velocity || !p.velocityDepth || p.velocityDepthSamples < 2) return false;
    if (p.debugView != 0 && p.debugView < 4) return false;
    // RndVelocityBuffer::AllocateData(w, h): SetBitmap(w / 2, h / 2, ...).
    const int vw = std::max(1, sceneW / 2), vh = std::max(1, sceneH / 2);
    if (!mVelTex || mVelW != vw || mVelH != vh) {
        Forget(mVelView);
        wgpu::TextureDescriptor td{};
        td.label = "DC3VelocityBuffer";
        td.size = {(uint32_t)vw, (uint32_t)vh, 1};
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::CopySrc;
        td.mipLevelCount = 1;
        mVelTex = gpu.CreateTexture(td);
        mVelView = mVelTex.CreateView();
        mVelW = vw;
        mVelH = vh;
    }
    DC3VelocityUniforms vu{};
    std::memcpy(vu.reproj, p.velocityReproj, sizeof(vu.reproj));
    vu.size[0] = (float)sceneW;
    vu.size[1] = (float)sceneH;
    vu.size[2] = (float)vw;
    vu.size[3] = (float)vh;
    const uint32_t off = NextSlot();
    gpu.Queue().WriteBuffer(mUniforms, off, &vu, sizeof(vu));
    wgpu::BindGroupEntry e[2] = {};
    e[0].binding = 11;
    e[0].textureView = p.velocityDepth;
    e[1].binding = 12;
    e[1].buffer = mUniforms;
    e[1].size = sizeof(vu);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3VelBGL;
    bgd.entryCount = 2;
    bgd.entries = e;
    wgpu::BindGroup bg = Group(gpu, bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = mVelView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.5, 0.5, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3Velocity";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    MILO_GPU_TIME_PASS(rp);
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mDC3VelPipe);
    pass.SetBindGroup(0, bg, 1, &off);
    pass.Draw(3);
    pass.End();
    mVelRun = mRunIndex + 1;   // the composite this Run is about to do
    mVelPasses++;
    return true;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
#include "gfx/ShippedWgsl.h"
const char* DC3RetailPostWgslSource() { return kDC3Shader; }
