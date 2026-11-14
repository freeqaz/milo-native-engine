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
//                    height/width. Retail's 0.5 texel is D3D9's half-texel
//                    convention; centred here, as RB3RetailPost's 15-tap blur.
//   postprocess.ps   bit 4 bloom (tf7 + tf11 + tf15, c6), bit 15 chroma (c121),
//                    bit 43 chroma sharpen, bit 2 noise (tf13, c112, c113), bit
//                    47 midtone noise, bit 21 colour matrix (c92..c94), bit 36
//                    vignette (c123). Bit numbers from RndShaderPostProc::
//                    CalcShaderOpts (dc3-decomp rndobj/Shader.cpp).
#include "gfx/RB3RetailPost.h"
#include "gfx/GpuDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
};
static_assert(sizeof(DC3CompositeUniforms) == 208, "DC3CompositeUniforms layout");

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

// bloom.ps with bilinear taps (tap[0..3].xy, weight 0.25 each): the scene
// weighted by its alpha, the pseudo-HDR mask.
@fragment fn fs_bloom_taps(in: VOut) -> @location(0) vec4f {
    var acc = vec3f(0.0);
    for (var i = 0; i < 4; i++) {
        let c = textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
        acc += c.rgb * c.a;
    }
    return vec4f(acc * 0.25, 1.0);
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
    return vec4f(2.0 - 20.0 / acc, 1.0);
}

// blur.ps with SetBloomBlurWeightsStreak's seven taps.
@fragment fn fs_streak(in: VOut) -> @location(0) vec4f {
    var acc = vec4f(0.0);
    for (var i = 0; i < 7; i++) {
        acc += tu.tap[i].z * textureSampleLevel(srcTex, samp, in.uv + tu.tap[i].xy, 0.0);
    }
    return acc;
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
    if (cu.motionMode.y > 0.5) {
        c = scene + 0.5 * cu.bloomColor.rgb * textureSampleLevel(bloom0, samp, uv, 0.0).rgb;
    } else if (cu.bloomColor.w > 0.5) {
        let b = textureSampleLevel(bloom0, samp, uv, 0.0).rgb
              + textureSampleLevel(bloom1, samp, uv, 0.0).rgb
              + textureSampleLevel(bloom2, samp, uv, 0.0).rgb;
        c = 1.0 - (1.0 - b * cu.bloomColor.rgb) * (1.0 - c);
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
    // Inspection views: the velocity target (tf10 as stored; (0.5, 0.5, 0)
    // where none ran), the bloom mask (scene alpha) and the bloom term.
    if (cu.bloomColor.w > 3.5) {
        if (cu.velocity.y > 0.5) {
            return vec4f(textureSampleLevel(velTex, samp, in.uv, 0.0).rgb, 1.0);
        }
        return vec4f(0.5, 0.5, 0.0, 1.0);
    }
    if (cu.bloomColor.w > 2.5) {
        let b = textureSampleLevel(bloom0, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom1, samp, in.uv, 0.0).rgb
              + textureSampleLevel(bloom2, samp, in.uv, 0.0).rgb;
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
    return vec4f(clamp(c, vec3f(0.0), vec3f(1.0)), 1.0);
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
    out.frame = vec4f(o, 1.0);
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
                mTex[s][k] = nullptr;
                mView[s][k] = nullptr;
            }
        }
    }
    EnsurePipelines(gpu);
    if (mDC3Ready && mDC3FrameFormat == gpu.SurfaceFormat()) return;
    auto& dev = gpu.Device();
    mDC3FrameFormat = gpu.SurfaceFormat();

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
        wgpu::BindGroupLayoutEntry e[10] = {texEntry(0), sampEntry(1), uniEntry(6, sizeof(DC3CompositeUniforms)),
                                            texEntry(3), texEntry(4), texEntry(5), texEntry(7), sampEntry(8),
                                            texEntry(9), texEntry(10)};
        wgpu::BindGroupLayoutDescriptor d{};
        d.entryCount = 10;
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
        return dev.CreateRenderPipeline(&pd);
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
        mDC3CompositeMBPipe = dev.CreateRenderPipeline(&pd);
    }
    mDC3Ready = true;
}

void RB3RetailPost::PassDC3(wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipe,
                            const wgpu::TextureView& dst, const wgpu::TextureView& src,
                            const float* uni, size_t uniBytes, GpuDevice& gpu) {
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, uni, uniBytes);

    wgpu::BindGroupEntry e[3] = {};
    e[0].binding = 0;
    e[0].textureView = src;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 2;
    e[2].buffer = mUniforms;
    e[2].offset = off;
    e[2].size = uniBytes;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3PassBGL;
    bgd.entryCount = 3;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = dst;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3RetailBloom";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

// SetBloomBlurWeightsStreak (dc3-decomp rndobj/Utl.cpp), as uv offsets.
static TapUniforms StreakTaps(bool horizontal, float w, float h, float attenuation, int pass,
                              float angleDeg, float yRatio) {
    float weights[7], offsets[7];
    const float scale = std::pow(4.0f, (float)pass);
    const float atten = std::pow(attenuation, scale);
    weights[3] = 1.0f / 3.0f;
    offsets[3] = 0.0f;
    float curWeight = atten;
    for (int i = 1; i <= 3; i++) {
        weights[3 - i] = weights[3 + i] = curWeight * (1.0f / 3.0f);
        offsets[3 - i] = -(float)i * scale;
        offsets[3 + i] = (float)i * scale;
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
    if (p.glare) {
        // DoBloom's glare arm (mBloomGlare, not the hi-res screen): bloom.ps
        // into set0, the plain 15-tap blur across and down, then one
        // bloom_glare.ps pass; set0 alone feeds the composite (tf7), whose
        // bit 37 adds it. Retail ping-pongs on one texture (tex[1] == tex[0]);
        // ordered here to end in [0][0].
        struct { float srcTexel[2], dir[2], lumaScale, pad[3]; } u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = 1.0f;
        Pass(encoder, mBloomDownPipe, mView[0][1], bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
        decltype(u) b{};
        b.srcTexel[0] = 1.0f / mW[0];
        b.srcTexel[1] = 1.0f / mH[0];
        b.dir[0] = 1.0f;
        Pass(encoder, mBlurPipe, mView[0][0], mView[0][1], &b.srcTexel[0], sizeof(b), gpu);
        b.dir[0] = 0.0f;
        b.dir[1] = 1.0f;
        Pass(encoder, mBlurPipe, mView[0][1], mView[0][0], &b.srcTexel[0], sizeof(b), gpu);
        TapUniforms t{};
        PassDC3(encoder, mDC3GlarePipe, mView[0][0], mView[0][1], &t.tap[0][0], sizeof(t), gpu);
        return;
    }
    if (!p.streak) {
        // The plain arm is RB3's: bloom.ps into set0, then 15-tap blurs and
        // downsample_4x into set1 and set2 (identical in DC3's DoBloom).
        struct { float srcTexel[2], dir[2], lumaScale, pad[3]; } u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = 1.0f;
        Pass(encoder, mBloomDownPipe, mView[0][0], bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
        for (int s = 0; s < kSets; s++) {
            if (s > 0) {
                decltype(u) d{};
                d.srcTexel[0] = 1.0f / mW[s - 1];
                d.srcTexel[1] = 1.0f / mH[s - 1];
                Pass(encoder, mDown4Pipe, mView[s][0], mView[s - 1][0], &d.srcTexel[0], sizeof(d), gpu);
            }
            decltype(u) b{};
            b.srcTexel[0] = 1.0f / mW[s];
            b.srcTexel[1] = 1.0f / mH[s];
            b.dir[0] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][1], mView[s][0], &b.srcTexel[0], sizeof(b), gpu);
            b.dir[0] = 0.0f;
            b.dir[1] = 1.0f;
            Pass(encoder, mBlurPipe, mView[s][0], mView[s][1], &b.srcTexel[0], sizeof(b), gpu);
        }
        return;
    }
    // The streak arm. set0: bloom.ps into w/4 (exact 4x4 box), three streak
    // passes along the rotated horizontal, ping-ponging to end in [0][0].
    {
        struct { float srcTexel[2], dir[2], lumaScale, pad[3]; } u{};
        u.srcTexel[0] = 1.0f / sceneW;
        u.srcTexel[1] = 1.0f / sceneH;
        u.lumaScale = 1.0f;
        Pass(encoder, mBloomDownPipe, mView[0][1], bloomSrc, &u.srcTexel[0], sizeof(u), gpu);
    }
    const wgpu::TextureView* ping[3][2] = {{&mView[0][1], &mView[0][0]},
                                           {&mView[0][0], &mView[0][1]},
                                           {&mView[0][1], &mView[0][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(true, (float)mW[0], (float)mH[0], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        PassDC3(encoder, mDC3StreakPipe, *ping[pass][1], *ping[pass][0], &t.tap[0][0], sizeof(t), gpu);
    }
    // set1: bloom.ps straight from the scene into w/16, then three passes
    // along the rotated vertical.
    {
        TapUniforms t{};
        const float tx = 2.0f / sceneW, ty = 2.0f / sceneH;
        const float o[4][2] = {{-tx, -ty}, {tx, ty}, {tx, -ty}, {-tx, ty}};
        for (int i = 0; i < 4; i++) {
            t.tap[i][0] = o[i][0];
            t.tap[i][1] = o[i][1];
            t.tap[i][2] = 0.25f;
        }
        PassDC3(encoder, mDC3BloomTapsPipe, mView[1][1], bloomSrc, &t.tap[0][0], sizeof(t), gpu);
    }
    const wgpu::TextureView* ping1[3][2] = {{&mView[1][1], &mView[1][0]},
                                            {&mView[1][0], &mView[1][1]},
                                            {&mView[1][1], &mView[1][0]}};
    for (int pass = 0; pass < 3; pass++) {
        TapUniforms t = StreakTaps(false, (float)mW[1], (float)mH[1], p.streakAttenuation, pass,
                                   p.streakAngle, p.screenAspect);
        PassDC3(encoder, mDC3StreakPipe, *ping1[pass][1], *ping1[pass][0], &t.tap[0][0], sizeof(t), gpu);
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
                     : p.bloom ? 1.0f : 0.0f;
    cu.vignette[0] = p.vignetteColor[0];
    cu.vignette[1] = p.vignetteColor[1];
    cu.vignette[2] = p.vignetteColor[2];
    cu.vignette[3] = p.vignetteIntensity;
    // CheckChromaticAberration: c121 = offset / (TheRnd.Width(), Height()).
    if (p.chromaticOffset != 0.0f) {
        cu.chroma[0] = p.chromaticOffset / (float)sceneW;
        cu.chroma[1] = p.chromaticOffset / (float)sceneH;
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
                wgpu::TextureDescriptor td{};
                td.label = "DC3PostProcessBuffer";
                td.size = {(uint32_t)sceneW, (uint32_t)sceneH, 1};
                td.format = wgpu::TextureFormat::RGBA8Unorm;
                td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
                td.mipLevelCount = 1;
                mPrevTex[i] = gpu.Device().CreateTexture(&td);
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
    const bool vel = p.velocity && (p.debugView == 0 || p.debugView == 4) && mVelView && mVelRun == run;
    if (vel) {
        cu.velocity[0] = p.velocityScale;
        cu.velocity[1] = 1.0f;
    }
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, &cu, sizeof(cu));

    // The plain arm fills all three sets; the streak arm two (tf15 black).
    const bool haveBloom = (p.bloom || p.debugView == 2) && mView[0][0];
    const int setsUsed = p.glare ? 1 : p.streak ? 2 : 3;
    wgpu::BindGroupEntry e[10] = {};
    e[0].binding = 0;
    e[0].textureView = sceneView;
    e[1].binding = 1;
    e[1].sampler = mSampler;
    e[2].binding = 6;
    e[2].buffer = mUniforms;
    e[2].offset = off;
    e[2].size = sizeof(cu);
    for (int s = 0; s < kSets; s++) {
        e[3 + s].binding = 3 + s;
        e[3 + s].textureView = (haveBloom && s < setsUsed) ? mView[s][0] : mBlackView;
    }
    e[6].binding = 7;
    e[6].textureView = haveNoise ? p.noiseMap : mBlackView;
    e[7].binding = 8;
    e[7].sampler = mNoiseSampler;
    e[8].binding = 9;
    e[8].textureView = prevRead;
    e[9].binding = 10;
    e[9].textureView = vel ? mVelView : mBlackView;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3CompositeBGL;
    bgd.entryCount = 10;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

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
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mb ? mDC3CompositeMBPipe : mDC3CompositePipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

// RndVelocityBuffer::Draw's camera pass (velocity_camera.ps) into tf10.
bool RB3RetailPost::RunDC3Velocity(wgpu::CommandEncoder& encoder, int sceneW, int sceneH,
                                   const RetailPostParams& p, GpuDevice& gpu) {
    if (!p.velocity || !p.velocityDepth || p.velocityDepthSamples < 2) return false;
    if (p.debugView != 0 && p.debugView != 4) return false;
    // RndVelocityBuffer::AllocateData(w, h): SetBitmap(w / 2, h / 2, ...).
    const int vw = std::max(1, sceneW / 2), vh = std::max(1, sceneH / 2);
    if (!mVelTex || mVelW != vw || mVelH != vh) {
        wgpu::TextureDescriptor td{};
        td.label = "DC3VelocityBuffer";
        td.size = {(uint32_t)vw, (uint32_t)vh, 1};
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::CopySrc;
        td.mipLevelCount = 1;
        mVelTex = gpu.Device().CreateTexture(&td);
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
    uint64_t off = (uint64_t)(mSlot++ % kSlots) * kSlotStride;
    gpu.Queue().WriteBuffer(mUniforms, off, &vu, sizeof(vu));
    wgpu::BindGroupEntry e[2] = {};
    e[0].binding = 11;
    e[0].textureView = p.velocityDepth;
    e[1].binding = 12;
    e[1].buffer = mUniforms;
    e[1].offset = off;
    e[1].size = sizeof(vu);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mDC3VelBGL;
    bgd.entryCount = 2;
    bgd.entries = e;
    wgpu::BindGroup bg = gpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment ca{};
    ca.view = mVelView;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.5, 0.5, 0, 0};
    wgpu::RenderPassDescriptor rp{};
    rp.label = "DC3Velocity";
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    auto pass = encoder.BeginRenderPass(&rp);
    pass.SetPipeline(mDC3VelPipe);
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
    mVelRun = mRunIndex + 1;   // the composite this Run is about to do
    mVelPasses++;
    return true;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
#include "gfx/ShippedWgsl.h"
const char* DC3RetailPostWgslSource() { return kDC3Shader; }
