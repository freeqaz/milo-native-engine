// SpotBeamPass — see SpotBeamPass.h for where each step comes from.
#include "gfx/SpotBeamPass.h"
#include "gfx/GpuDevice.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Retail tunables (rb3-xenon world/SpotlightDrawer_NG.cpp statics, values read
// from the retail image's .data).
constexpr float kBeamIntensity = 8.0f;    // sBeamIntensity     0x82C711BC
constexpr float kBeamBrighten = 0.1f;     // sBeamBrighten      0x82C7118C
constexpr float kSphereScale = 1.0f;      // sSphereScale       0x82C71190
constexpr float kSheetIntensity = 8.0f;   // sSheetIntensity    0x82C71194
constexpr float kSheetW = 0.5f;           // sSheetW            0x82C71198
constexpr float kFogScale = 0.125f;       // RenderScene c9     0x82C711CC
constexpr float kPostIntensity = 32.0f;   // sPostIntensityScale 0x82C711C8

// One beam's uniform block. Every member is a vec4 (or a mat4), so the WGSL
// struct below has the same layout.
struct BeamUniforms {
    float vp[16];
    float ivp[16];
    float m[3][4];       // world.i = dot(m[i], (pos, 1))
    float c10[4], c25[4], c26[4], c27[4], c28[4], c30[4];
    float c86[4], c87[4], c88[4], c89[4], c90[4], c91[4], c127[4], c9[4];
    float rt[4];         // 1/w, 1/h of the beam target; zRange.x, zRange.y - zRange.x
    float misc[4];       // x: the sheet's tex12 linear depth (tex12 is black)
};
static_assert(sizeof(BeamUniforms) == 432, "BeamUniforms layout");

struct BlurUniforms {
    float tap[5][4];     // xy offset (uv), z weight
    float invSize[4];
};
static_assert(sizeof(BlurUniforms) == 96, "BlurUniforms layout");

constexpr uint64_t kSlot = 512;  // >= minUniformBufferOffsetAlignment, >= both blocks

const char* kShader = R"WGSL(
struct BeamUB {
    vp: mat4x4f,
    ivp: mat4x4f,
    m0: vec4f, m1: vec4f, m2: vec4f,
    c10: vec4f, c25: vec4f, c26: vec4f, c27: vec4f, c28: vec4f, c30: vec4f,
    c86: vec4f, c87: vec4f, c88: vec4f, c89: vec4f, c90: vec4f, c91: vec4f,
    c127: vec4f, c9: vec4f,
    rt: vec4f,
    misc: vec4f,
};
@group(0) @binding(0) var<uniform> u: BeamUB;
@group(0) @binding(1) var depthTex: texture_depth_multisampled_2d;
@group(0) @binding(2) var xsecTex: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) world: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

// depthvolume.vs: world = c92..c94 * pos, clip = world * GetInfiniteViewProj.
// Here the scene's projection, whose x, y and w are the infinite one's; z is
// held inside the far plane, which the infinite projection does not have
// (nothing tests or writes depth), and keeps the near clip.
@vertex fn vs_beam(@location(0) pos: vec3f, @location(1) color: vec4f,
                   @location(2) uv: vec2f) -> VOut {
    let p = vec4f(pos, 1.0);
    let world = vec3f(dot(u.m0, p), dot(u.m1, p), dot(u.m2, p));
    var clip = u.vp * vec4f(world, 1.0);
    clip.z = min(clip.z, clip.w);
    var o: VOut;
    o.pos = clip;
    o.world = world;
    o.uv = uv;
    o.color = color;
    return o;
}

// The view depth of a world point (c30: the view plane).
fn viewDepth(p: vec3f) -> f32 { return dot(p, u.c30.xyz) + u.c30.w; }

// tf9 (the scene's depth, point-sampled at the screen position) as linear view
// depth. Retail decodes its reverse-Z value through c89; this renderer's depth
// is the scene projection's, through the camera's viewport depth range, so it
// is unprojected instead.
fn sceneDepth(uv: vec2f) -> f32 {
    let dims = vec2i(textureDimensions(depthTex));
    let px = clamp(vec2i(uv * vec2f(dims)), vec2i(0), dims - 1);
    let d = textureLoad(depthTex, px, 0);
    let ndc = vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (d - u.rt.z) / u.rt.w, 1.0);
    let w = u.ivp * ndc;
    return viewDepth(w.xyz / w.w);
}

// The fog factor every depthvolume shader ends with. `dens` is tf5's density
// (green), here 0: c127.zw are 0 while the beams draw (SetupFogDensityState),
// which removes the density from every result anyway.
fn fogKeep(vd: f32) -> f32 {
    let dens = 0.0;
    let a = (1.0 - clamp(u.c9.x / (u.c9.x + dens), 0.0, 1.0)) * u.c127.w + u.c127.z;
    let b = clamp(u.c127.y * (vd - u.c127.x), 0.0, 1.0);
    return 1.0 - clamp(b * a, 0.0, 1.0);
}

// depthvolume.ps, option 0 (cone).
@fragment fn fs_cone(in: VOut) -> @location(0) vec4f {
    let uv = in.pos.xy * u.rt.xy;
    let eye = u.c10.xyz;
    let ax = u.c26.xyz;
    let rc = u.c27.xyz;
    let dir = in.world - eye;
    let dist2 = dot(dir, dir);
    let nd = dir * inverseSqrt(abs(dist2));
    let cos2 = u.c28.w;
    let axRc = dot(ax, rc);
    let ndAx = dot(nd, ax);
    // The ray eye + t * nd against the cone (q.ax)^2 = cos^2 |q|^2, q = p - apex:
    // a t^2 + 2 b t + c = 0.
    let a = ndAx * ndAx - cos2;
    let b = ndAx * axRc - cos2 * dot(nd, rc);
    let c = axRc * axRc - cos2 * dot(rc, rc);
    let maxT = min(sceneDepth(uv), sqrt(abs(dist2)));
    let disc = b * b - c * a;
    var enterP = eye;
    var exitP = eye;
    if (disc > 0.0) {
        let s = sqrt(abs(disc));
        let ra = 1.0 / a;
        let t1 = (-b - s) * ra;
        let t2 = (-b + s) * ra;
        // Which roots lie on the beam's half of the double cone.
        let h1 = dot(rc + t1 * nd, ax);
        let h2 = dot(rc + t2 * nd, ax);
        var x = t1;
        var y = t2;
        if (h1 > 0.0 && h2 <= 0.0) {
            y = 0.0;
        } else {
            if (!(h2 > 0.0 && h1 > 0.0)) { x = t2; }
            if (h2 > 0.0 && h1 <= 0.0) { x = maxT; }
        }
        x = min(maxT, max(x, 0.0));
        y = min(maxT, max(y, 0.0));
        enterP = eye + nd * x;
        exitP = eye + nd * y;
    }
    // Cross section: where the pixel lies between the silhouette planes.
    let rD = dot(in.world, u.c87.xyz) - u.c87.w;
    let lD = u.c88.w - dot(in.world, u.c88.xyz);
    let xu = 2.0 * rD / (lD + rD) - 1.0;
    let xs = textureSampleLevel(xsecTex, samp, vec2f(abs(xu), 0.0), 0.0).r;
    let xsf = (1.0 - u.c86.x) + xs * u.c86.x;
    // Falloff (1 - s)^2 along the axis, averaged over the segment.
    let sE = clamp(dot(enterP - u.c25.xyz, ax) * u.c25.w, 0.0, 1.0);
    let sX = clamp(dot(exitP - u.c25.xyz, ax) * u.c25.w, 0.0, 1.0);
    let oE = 1.0 - sE;
    let oX = 1.0 - sX;
    let ds = sX - sE;
    var fall = 0.004 * oE * oE;   // the limit of the mean as ds -> 0
    if (abs(ds) > 1e-6) {
        fall = 0.004 * ((oE * oE * oE - oX * oX * oX) / 3.0) / ds;
    }
    let len = abs(viewDepth(exitP) - viewDepth(enterP));
    let k = fall * len * xsf * fogKeep(viewDepth(in.world));
    return vec4f(u.c90.rgb * k, 0.0);
}

// depthvolume.ps, option 2 (sheet). tf12 is SpotlightResources' black texture
// while the beams draw; its linear depth is u.misc.x.
@fragment fn fs_sheet(in: VOut) -> @location(0) vec4f {
    let uv = in.pos.xy * u.rt.xy;
    let dir = in.world - u.c10.xyz;
    let nd = dir * inverseSqrt(abs(dot(dir, dir)));
    let lz = dot(nd, u.c91.xyz) + 0.01;
    let xs = textureSampleLevel(xsecTex, samp, in.uv, 0.0).r;
    let vd = viewDepth(in.world);
    let visible = select(0.0, 1.0, sceneDepth(uv) >= vd);
    let k12 = select(1.0, 0.625, vd > u.misc.x);
    let keep = fogKeep(vd);
    let k = xs * in.color.a * k12 * visible * keep / abs(lz) * u.c91.w;
    return vec4f(u.c90.rgb * k, 0.0);
}

// depthvolume.ps, option 4 (sphere): a slab tex11 * c91.w thick around the
// billboard, clipped by the scene depth.
@fragment fn fs_sphere(in: VOut) -> @location(0) vec4f {
    let uv = in.pos.xy * u.rt.xy;
    let thick = textureSampleLevel(xsecTex, samp, in.uv, 0.0).r * u.c91.w;
    let vd = viewDepth(in.world);
    let scene = sceneDepth(uv);
    let t = min(vd + thick, scene) - min(vd - thick, scene);
    let keep = fogKeep(vd);
    return vec4f(u.c90.rgb * (0.625 * t * keep), 0.0);
}

// blur.ps option 0x10000: five taps.
struct BlurUB {
    tap: array<vec4f, 5>,
    invSize: vec4f,
};
@group(0) @binding(0) var<uniform> bu: BlurUB;
@group(0) @binding(1) var blurSrc: texture_2d<f32>;
@group(0) @binding(2) var blurSamp: sampler;

@vertex fn vs_full(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4f {
    let x = f32(i32(idx & 1u)) * 4.0 - 1.0;
    let y = f32(i32(idx >> 1u)) * 4.0 - 1.0;
    return vec4f(x, y, 0.0, 1.0);
}

@fragment fn fs_blur(@builtin(position) p: vec4f) -> @location(0) vec4f {
    let uv = p.xy * bu.invSize.xy;
    var acc = vec4f(0.0);
    for (var i = 0; i < 5; i++) {
        acc += bu.tap[i].z * textureSampleLevel(blurSrc, blurSamp, uv + bu.tap[i].xy, 0.0);
    }
    return acc;
}
)WGSL";

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline void Normalize3(float* v) {
    float l = std::sqrt(Dot3(v, v));
    if (l > 0.0f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}
inline void Set4(float* o, float x, float y, float z, float w) {
    o[0] = x;
    o[1] = y;
    o[2] = z;
    o[3] = w;
}

bool Finite(const SpotBeamPass::Constants& k) {
    const float* p = &k.c10[0];
    const float* end = &k.c9[3];
    for (; p <= end; p++)
        if (!std::isfinite(*p)) return false;
    return true;
}

// 4x4 inverse (row-major), for unprojecting the scene depth.
bool Invert4(const float* m, float* inv) {
    float t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (det == 0.0f || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; i++) inv[i] = t[i] / det;
    return true;
}

}  // namespace

const float SpotBeamPass::kBlurWeights[5] = {0.1f, 0.25f, 0.3f, 0.25f, 0.1f};

// SetupXSection: the beam's two silhouette planes as seen from the eye, and
// the view-angle fade of the cross section.
static void XSection(const NativeSpotBeam& b, const SpotBeamPass::Camera& cam,
                     SpotBeamPass::Constants& k) {
    const float* lp = b.lightPos;
    const float* beamDir = b.axis;
    float viewDir[3] = {lp[0] - cam.pos[0], lp[1] - cam.pos[1], lp[2] - cam.pos[2]};
    Normalize3(viewDir);
    float perp[3];
    Cross3(viewDir, beamDir, perp);
    Normalize3(perp);
    const float topR = b.ngRadii[0], botR = b.ngRadii[1], len = b.length;
    float topRight[3], topLeft[3], botRight[3], botLeft[3], botCenter[3];
    for (int i = 0; i < 3; i++) {
        topRight[i] = lp[i] + perp[i] * topR;
        botCenter[i] = lp[i] + beamDir[i] * len;
        topLeft[i] = lp[i] - perp[i] * topR;
        botRight[i] = botCenter[i] + perp[i] * botR;
        botLeft[i] = botCenter[i] - perp[i] * botR;
    }
    const float* eye = cam.pos;
    auto plane = [&](const float* p0, const float* p1, float* n) {
        float a[3] = {p0[0] - eye[0], p0[1] - eye[1], p0[2] - eye[2]};
        float c[3] = {p1[0] - eye[0], p1[1] - eye[1], p1[2] - eye[2]};
        Cross3(a, c, n);
        Normalize3(n);
    };
    float rightPlane[3], leftPlane[3];
    plane(topRight, botRight, rightPlane);
    plane(topLeft, botLeft, leftPlane);
    const float rightD = Dot3(eye, rightPlane);
    const float leftD = Dot3(eye, leftPlane);
    float axisV[3] = {rightPlane[0] + leftPlane[0], rightPlane[1] + leftPlane[1],
                      rightPlane[2] + leftPlane[2]};
    Normalize3(axisV);
    const float rightCos = Dot3(rightPlane, axisV);
    const float invRight = rightCos == 0.0f ? 0.0f : 1.0f / rightCos;
    const float leftCos = Dot3(leftPlane, axisV);
    const float invLeft = leftCos == 0.0f ? 0.0f : 1.0f / leftCos;
    Set4(k.c88, leftPlane[0] * invLeft, leftPlane[1] * invLeft, leftPlane[2] * invLeft,
         invLeft * leftD);
    Set4(k.c87, rightPlane[0] * invRight, rightPlane[1] * invRight, rightPlane[2] * invRight,
         invRight * rightD);

    const float minR = (topR - botR) >= 0.0f ? botR : topR;
    const float apexDist = 0.0f < botR ? (len * minR) / (botR - minR) : 0.0f;
    const float halfAngle = (botR * 0.5f) / (len + apexDist);
    float axisDot = Dot3(beamDir, viewDir);
    const float sq = halfAngle * halfAngle;
    const float fade = (1.0f - sq) / (sq + 1.0f);
    if (axisDot <= 0.0f) axisDot = -axisDot;
    float vis;
    if (axisDot < fade) {
        const float slack = fade - axisDot;
        if (slack < 0.02f) {
            const float t = -(slack * 50.0f - 1.0f);
            vis = -(t * t * t * t - 1.0f);
        } else {
            vis = 1.0f;
        }
    } else {
        vis = 0.0f;
    }
    Set4(k.c86, vis, 0.0f, 0.0f, 0.0f);
}

bool SpotBeamPass::BeamConstants(const NativeSpotBeam& b, const Camera& cam, Constants& k) {
    k = Constants{};
    // RenderBeams: only beams with length draw.
    if (!(b.length > 0.0f)) return false;
    const float far = cam.farPlane;
    const float invFar = far > 0.0f ? 1.0f / far : 0.0f;
    // RenderScene / SetupFogDensityState, shared by every beam.
    Set4(k.c9, kFogScale, kFogScale, kFogScale, kFogScale);
    Set4(k.c127, 0.0f, invFar, 0.0f, 0.0f);
    const float zr = 1.0f / (cam.zRange[1] - cam.zRange[0]);
    Set4(k.c89, cam.nearPlane, far, zr, zr * cam.zRange[0]);
    Set4(k.c10, cam.pos[0], cam.pos[1], cam.pos[2], 1.0f);

    switch (b.shape) {
    case 2: {  // RenderSheet
        k.shape = 1;
        Set4(k.c91, b.sheetDir[0], b.sheetDir[1], b.sheetDir[2], kSheetW);
        const float s = b.intensity * kSheetIntensity;
        float r = b.color[0] * s, g = b.color[1] * s, bl = b.color[2] * s;
        r *= b.matColor[0];
        g *= b.matColor[1];
        bl *= b.matColor[2];
        Set4(k.c90, r * b.brighten, g * b.brighten, bl * b.brighten, 1.0f);
        break;
    }
    case 3:
    case 4: {  // RenderSphere
        k.shape = 2;
        Set4(k.c91, 0.0f, 0.0f, 0.625f, b.topRadius * kSphereScale);
        const float s = b.intensity * b.brighten * kBeamBrighten;
        Set4(k.c90, s * b.color[0] * b.matColor[0], b.color[1] * s * b.matColor[1],
             b.color[2] * s * b.matColor[2], b.color[3] * s * b.matColor[3]);
        break;
    }
    default: {  // RenderCone -> RenderConeDefs
        k.shape = 0;
        const float s = b.intensity * kBeamIntensity;
        float color[4];
        for (int i = 0; i < 4; i++) color[i] = b.color[i] * s * b.matColor[i];
        Set4(k.c90, color[0] * b.brighten, color[1] * b.brighten, color[2] * b.brighten, 1.0f);
        XSection(b, cam, k);
        const float* fwd = cam.fwd;
        Set4(k.c30, fwd[0], fwd[1], fwd[2],
             -(fwd[0] * cam.pos[0] + (fwd[1] * cam.pos[1] + fwd[2] * cam.pos[2])));
        // c91 = (0, mHalfDistance, 0, 1/far): the cone shader does not read it.
        Set4(k.c91, 0.0f, 0.0f, 0.0f, invFar);
        const float topRad = b.ngRadii[0], botRad = b.ngRadii[1];
        const float minRad = (topRad - botRad) < 0.0f ? topRad : botRad;
        const float offset = 0.0f < botRad ? (minRad * b.length) / (botRad - minRad) : 0.0f;
        const float totalLength = offset + b.length;
        const float invTotalLength = 1.0f / totalLength;
        float apex[3];
        for (int i = 0; i < 3; i++) apex[i] = b.axis[i] * -offset + b.lightPos[i];
        Set4(k.c25, apex[0], apex[1], apex[2], invTotalLength);
        Set4(k.c26, b.axis[0], b.axis[1], b.axis[2], totalLength);
        const float rel[3] = {cam.pos[0] - apex[0], cam.pos[1] - apex[1], cam.pos[2] - apex[2]};
        Set4(k.c27, rel[0], rel[1], rel[2], 1.0f);
        const float radiusDiff = botRad - minRad;
        const float dotRelDir = Dot3(b.axis, rel);
        const float tanSlope = invTotalLength * radiusDiff;
        const float shift = radiusDiff != 0.0f ? (minRad / radiusDiff) * totalLength : 0.0f;
        const float cosAngle = std::cos(std::atan(invTotalLength * botRad));
        const float extProj = shift + dotRelDir;
        Set4(k.c28, tanSlope * tanSlope + 1.0f, extProj * tanSlope * tanSlope + dotRelDir,
             -(extProj * extProj * tanSlope * tanSlope -
               -(dotRelDir * dotRelDir - Dot3(rel, rel))),
             cosAngle * cosAngle);
        break;
    }
    }
    if (k.shape != 0) {
        const float* fwd = cam.fwd;
        Set4(k.c30, fwd[0], fwd[1], fwd[2], -Dot3(fwd, cam.pos));
    }
    return Finite(k);
}

void SpotBeamPass::CompositeConstants(const NativeSpotBeamFrame& f, float out[3]) {
    // SetupForPostProcess: c91.x; SetupFogDensityMap: c127.xy.
    out[0] = f.intensity * kPostIntensity;
    const float base = f.baseIntensity * 0.01f;
    float smoke = f.smokeIntensity * 0.01f;
    smoke *= 1.0f - base;
    out[1] = base;
    out[2] = smoke;
}

void SpotBeamPass::BlurOffsets(float amountX, float amountY, int w, int h, float out[5][2]) {
    const float invW = 1.0f / (float)w, invH = 1.0f / (float)h;
    for (int i = -2; i <= 2; i++) {
        out[i + 2][0] = (float)i * invW * amountX;
        out[i + 2][1] = (float)i * invH * amountY;
    }
}

bool SpotBeamPass::EnsurePipelines(GpuDevice& gpu) {
    if (mReady) return true;
    if (mFailed) return false;
    wgpu::Device& dev = gpu.Device();

    wgpu::ShaderSourceWGSL src;
    src.code = kShader;
    wgpu::ShaderModuleDescriptor sm{};
    sm.nextInChain = &src;
    sm.label = "SpotBeam";
    mShader = dev.CreateShaderModule(&sm);

    {
        wgpu::BindGroupLayoutEntry e[4] = {};
        e[0].binding = 0;
        e[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        e[0].buffer.type = wgpu::BufferBindingType::Uniform;
        e[0].buffer.minBindingSize = sizeof(BeamUniforms);
        e[1].binding = 1;
        e[1].visibility = wgpu::ShaderStage::Fragment;
        e[1].texture.sampleType = wgpu::TextureSampleType::Depth;
        e[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[1].texture.multisampled = true;
        e[2].binding = 2;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].texture.sampleType = wgpu::TextureSampleType::Float;
        e[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[3].binding = 3;
        e[3].visibility = wgpu::ShaderStage::Fragment;
        e[3].sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor d{};
        d.label = "SpotBeam";
        d.entryCount = 4;
        d.entries = e;
        mBeamBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::BindGroupLayoutEntry e[3] = {};
        e[0].binding = 0;
        e[0].visibility = wgpu::ShaderStage::Fragment;
        e[0].buffer.type = wgpu::BufferBindingType::Uniform;
        e[0].buffer.minBindingSize = sizeof(BlurUniforms);
        e[1].binding = 1;
        e[1].visibility = wgpu::ShaderStage::Fragment;
        e[1].texture.sampleType = wgpu::TextureSampleType::Float;
        e[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[2].binding = 2;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor d{};
        d.label = "SpotBeamBlur";
        d.entryCount = 3;
        d.entries = e;
        mBlurBGL = dev.CreateBindGroupLayout(&d);
    }
    {
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 1;
        d.bindGroupLayouts = &mBeamBGL;
        mBeamPL = dev.CreatePipelineLayout(&d);
        d.bindGroupLayouts = &mBlurBGL;
        mBlurPL = dev.CreatePipelineLayout(&d);
    }

    const wgpu::TextureFormat fmt = wgpu::TextureFormat::RGBA8Unorm;  // A8R8G8B8
    wgpu::ColorTargetState ct{};
    ct.format = fmt;
    ct.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState fs{};
    fs.module = mShader;
    fs.targetCount = 1;
    fs.targets = &ct;

    // RndShaderDepthVolume::Select: blend ONE/ONE, add.
    wgpu::BlendState add{};
    add.color.operation = wgpu::BlendOperation::Add;
    add.color.srcFactor = wgpu::BlendFactor::One;
    add.color.dstFactor = wgpu::BlendFactor::One;
    add.alpha = add.color;
    ct.blend = &add;

    wgpu::VertexAttribute attrs[3] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x3;
    attrs[0].offset = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x4;
    attrs[1].offset = 24;
    attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Float32x2;
    attrs[2].offset = 40;
    attrs[2].shaderLocation = 2;
    wgpu::VertexBufferLayout vbl{};
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 3;
    vbl.attributes = attrs;

    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = mBeamPL;
    pd.vertex.module = mShader;
    pd.vertex.entryPoint = "vs_beam";
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vbl;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.primitive.frontFace = wgpu::FrontFace::CCW;  // as PipelineManager
    pd.multisample.count = 1;
    pd.fragment = &fs;
    const char* entry[3] = {"fs_cone", "fs_sheet", "fs_sphere"};
    // Cone: cull override 3 (D3DCULL_CCW, the reverse of a material's
    // D3DCULL_CW): the shaft's far side draws. Sheet and sphere: override 1.
    const wgpu::CullMode cull[3] = {wgpu::CullMode::Front, wgpu::CullMode::None,
                                    wgpu::CullMode::None};
    const uint32_t strides[2] = {64, 88};
    for (int s = 0; s < 3; s++) {
        for (int v = 0; v < 2; v++) {
            fs.entryPoint = entry[s];
            pd.primitive.cullMode = cull[s];
            vbl.arrayStride = strides[v];
            pd.label = entry[s];
            mBeamPipe[s][v] = dev.CreateRenderPipeline(&pd);
            if (!mBeamPipe[s][v]) {
                mFailed = true;
                return false;
            }
        }
    }

    // BlurRT: DrawRect with the work material, kBlendSrc.
    ct.blend = nullptr;
    fs.entryPoint = "fs_blur";
    wgpu::RenderPipelineDescriptor bd{};
    bd.label = "SpotBeamBlur";
    bd.layout = mBlurPL;
    bd.vertex.module = mShader;
    bd.vertex.entryPoint = "vs_full";
    bd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    bd.fragment = &fs;
    mBlurPipe = dev.CreateRenderPipeline(&bd);
    if (!mBlurPipe) {
        mFailed = true;
        return false;
    }

    // Retail SetXSectionTexture / BlurRT: linear filter (mode 1), clamp (mode 2).
    SamplerDesc sd{};
    sd.addressU = wgpu::AddressMode::ClampToEdge;
    sd.addressV = wgpu::AddressMode::ClampToEdge;
    mLinear = gpu.GetSampler(sd);

    // SR().unk14: Rnd::kDefaultTex_WhiteTransparent stands in for a missing
    // cross section; only its red channel is read.
    wgpu::TextureDescriptor td{};
    td.label = "SpotBeamWhite";
    td.size = {1, 1, 1};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    mWhiteTex = dev.CreateTexture(&td);
    const uint8_t white[4] = {255, 255, 255, 0};
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = mWhiteTex;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D ext = {1, 1, 1};
    gpu.Queue().WriteTexture(&dst, white, sizeof(white), &layout, &ext);
    mWhiteView = mWhiteTex.CreateView();

    mReady = true;
    return true;
}

void SpotBeamPass::EnsureTargets(int w, int h, GpuDevice& gpu) {
    if (mW == w && mH == h && mTex[0]) return;
    for (int i = 0; i < 2; i++) {
        wgpu::TextureDescriptor td{};
        td.label = "SpotBeamTarget";
        td.size = {(uint32_t)w, (uint32_t)h, 1};
        td.format = wgpu::TextureFormat::RGBA8Unorm;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::CopySrc;
        mTex[i] = gpu.Device().CreateTexture(&td);
        mView[i] = mTex[i].CreateView();
    }
    mW = w;
    mH = h;
}

bool SpotBeamPass::Run(wgpu::CommandEncoder& encoder, const Camera& cam, const Draw* draws,
                       size_t count, const wgpu::TextureView& depthView, uint32_t depthSamples,
                       int sceneW, int sceneH, GpuDevice& gpu) {
    if (!encoder || !count || !depthView || sceneW < 2 || sceneH < 2) return false;
    if (depthSamples < 2) {
        if (!mWarnedSamples) {
            fprintf(stderr, "SpotBeamPass: single-sample depth is not supported; no beams\n");
            mWarnedSamples = true;
        }
        return false;
    }
    if (!EnsurePipelines(gpu)) return false;
    float ivp[16];
    if (!Invert4(cam.viewProj, ivp)) return false;

    // RTWidth / RTHeight: half the pre-process texture.
    const int w = sceneW >> 1, h = sceneH >> 1;
    EnsureTargets(w, h, gpu);
    wgpu::Device& dev = gpu.Device();

    // One uniform slot per beam, then the two blur passes. Queue::WriteBuffer
    // lands at submit, ahead of every pass, so nothing is shared.
    const size_t slots = count + 2;
    std::vector<uint8_t> data(slots * kSlot, 0);
    for (size_t i = 0; i < count; i++) {
        const Draw& d = draws[i];
        BeamUniforms bu{};
        std::memcpy(bu.vp, cam.viewProj, sizeof(bu.vp));
        std::memcpy(bu.ivp, ivp, sizeof(bu.ivp));
        // world = pos.x * m.x + pos.y * m.y + pos.z * m.z + v
        for (int r = 0; r < 3; r++) {
            bu.m[r][0] = d.meshXfm[0 + r];
            bu.m[r][1] = d.meshXfm[3 + r];
            bu.m[r][2] = d.meshXfm[6 + r];
            bu.m[r][3] = d.meshXfm[9 + r];
        }
        const Constants& k = d.k;
        std::memcpy(bu.c10, k.c10, 16);
        std::memcpy(bu.c25, k.c25, 16);
        std::memcpy(bu.c26, k.c26, 16);
        std::memcpy(bu.c27, k.c27, 16);
        std::memcpy(bu.c28, k.c28, 16);
        std::memcpy(bu.c30, k.c30, 16);
        std::memcpy(bu.c86, k.c86, 16);
        std::memcpy(bu.c87, k.c87, 16);
        std::memcpy(bu.c88, k.c88, 16);
        std::memcpy(bu.c89, k.c89, 16);
        std::memcpy(bu.c90, k.c90, 16);
        std::memcpy(bu.c91, k.c91, 16);
        std::memcpy(bu.c127, k.c127, 16);
        std::memcpy(bu.c9, k.c9, 16);
        Set4(bu.rt, 1.0f / (float)w, 1.0f / (float)h, cam.zRange[0],
             cam.zRange[1] - cam.zRange[0]);
        // tex12 (black) read as reverse-Z depth 0, decoded through c89.
        const float z12 = (1.0f - 0.0f) * k.c89[2] - k.c89[3];
        bu.misc[0] = k.c89[0] * k.c89[1] / (k.c89[1] - (k.c89[1] - k.c89[0]) * z12);
        std::memcpy(&data[i * kSlot], &bu, sizeof(bu));
    }
    // BlurRT(): sSeparateBlurPasses, sBlurAmount 1: along x, then along y.
    for (int pass = 0; pass < 2; pass++) {
        float off[5][2];
        BlurOffsets(pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, w, h, off);
        BlurUniforms b{};
        for (int t = 0; t < 5; t++) Set4(b.tap[t], off[t][0], off[t][1], kBlurWeights[t], 0.0f);
        Set4(b.invSize, 1.0f / (float)w, 1.0f / (float)h, 0.0f, 0.0f);
        std::memcpy(&data[(count + pass) * kSlot], &b, sizeof(b));
    }
    wgpu::BufferDescriptor bd{};
    bd.label = "SpotBeamUniforms";
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    bd.size = data.size();
    wgpu::Buffer ubuf = dev.CreateBuffer(&bd);
    gpu.Queue().WriteBuffer(ubuf, 0, data.data(), data.size());

    // RenderScene: mSpotCam selects SR().unk8, and DxCam::Select clears a
    // kDepthVolumeMap target to 0xFF000000.
    {
        wgpu::RenderPassColorAttachment ca{};
        ca.view = mView[0];
        ca.loadOp = wgpu::LoadOp::Clear;
        ca.storeOp = wgpu::StoreOp::Store;
        ca.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor rp{};
        rp.label = "SpotBeams";
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
        pass.SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
        for (size_t i = 0; i < count; i++) {
            const Draw& d = draws[i];
            if (!d.vertexBuffer || !d.indexBuffer || !d.indexCount) continue;
            if (d.vertexStride != 64 && d.vertexStride != 88) continue;
            wgpu::BindGroupEntry e[4] = {};
            e[0].binding = 0;
            e[0].buffer = ubuf;
            e[0].offset = i * kSlot;
            e[0].size = sizeof(BeamUniforms);
            e[1].binding = 1;
            e[1].textureView = depthView;
            e[2].binding = 2;
            e[2].textureView = d.xsection ? d.xsection : mWhiteView;
            e[3].binding = 3;
            e[3].sampler = mLinear;
            wgpu::BindGroupDescriptor g{};
            g.label = "SpotBeam";
            g.layout = mBeamBGL;
            g.entryCount = 4;
            g.entries = e;
            pass.SetPipeline(mBeamPipe[d.k.shape][d.vertexStride == 64 ? 0 : 1]);
            pass.SetBindGroup(0, dev.CreateBindGroup(&g));
            pass.SetVertexBuffer(0, d.vertexBuffer, 0, d.vertexBytes);
            pass.SetIndexBuffer(d.indexBuffer, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
            pass.DrawIndexed(d.indexCount, 1, 0, 0, 0);
        }
        pass.End();
    }
    // BlurRT twice: 0 -> 1 along x, 1 -> 0 along y (retail resolves in place).
    for (int p = 0; p < 2; p++) {
        wgpu::BindGroupEntry e[3] = {};
        e[0].binding = 0;
        e[0].buffer = ubuf;
        e[0].offset = (count + p) * kSlot;
        e[0].size = sizeof(BlurUniforms);
        e[1].binding = 1;
        e[1].textureView = mView[p];
        e[2].binding = 2;
        e[2].sampler = mLinear;
        wgpu::BindGroupDescriptor g{};
        g.label = "SpotBeamBlur";
        g.layout = mBlurBGL;
        g.entryCount = 3;
        g.entries = e;
        wgpu::BindGroup bg = dev.CreateBindGroup(&g);
        wgpu::RenderPassColorAttachment ca{};
        ca.view = mView[1 - p];
        ca.loadOp = wgpu::LoadOp::Clear;
        ca.storeOp = wgpu::StoreOp::Store;
        ca.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor rp{};
        rp.label = "SpotBeamBlur";
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
        pass.SetPipeline(mBlurPipe);
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    }
    return true;
}

void SpotBeamPass::Terminate() {
    for (int s = 0; s < 3; s++)
        for (int v = 0; v < 2; v++) mBeamPipe[s][v] = nullptr;
    mBlurPipe = nullptr;
    mBeamPL = mBlurPL = nullptr;
    mBeamBGL = mBlurBGL = nullptr;
    mShader = nullptr;
    mLinear = nullptr;
    mWhiteTex = nullptr;
    mWhiteView = nullptr;
    for (int i = 0; i < 2; i++) {
        mTex[i] = nullptr;
        mView[i] = nullptr;
    }
    mW = mH = 0;
    mReady = false;
    mFailed = false;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h).
#include "gfx/ShippedWgsl.h"
const char* SpotBeamPassWgslSource() { return kShader; }
