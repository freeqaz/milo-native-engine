// CPU-side uniform structs — the WGSL-layout contract for the engine's
// standard shader (src/gfx/standard_wgsl.inc).
//
// Decomp-agnostic by construction: plain floats only, no rndobj/ types. Shared
// between every gfx backend so all backends speak the same shader contract.
// Backends today:
//   - src/platform/Rnd_Wgpu.h      (WgpuRnd : NgRnd, DC3-shaped rndobj)
//   - src/platform/Rnd_Wgpu_RB3.h  (BandRnd : Rnd, RB3-shaped rndobj)
//
// The static_asserts here are the load-bearing invariant: if a backend wants to
// extend a struct, it must extend it HERE so all backends and the WGSL stay in
// lockstep.

#pragma once

#include <cstdint>

struct SceneUniforms {
    float viewProj[16];       // mat4x4f
    float view[16];           // mat4x4f
    float cameraPos[3];       // vec3f
    float _pad0;
    float fogColor[3];        // vec3f
    float fogStart;
    float fogEnd;
    float fogEnabled;
    float _pad1[2];
    float lightDirs[4][4];    // array<vec4f, 4> — direction per light
    float lightColors[4][4];  // array<vec4f, 4> — color per light
    float ambientColor[4];    // vec4f
    float numLights;          // f32
    float _padN[3];
    // Point lights (up to 4)
    float pointLightPos[4][4];    // array<vec4f, 4> — world position (.w unused)
    float pointLightColors[4][4]; // array<vec4f, 4> — color per light
    float pointLightRanges[4];    // vec4f — falloff range per light
    float numPointLights;         // f32
    float _padPL[3];
    // Shadow mapping
    float lightViewProj[16];      // mat4x4f — light's VP for shadow lookup
    float shadowEnabled;           // f32 — 1.0 when shadow map valid
    float shadowBias;              // f32 — depth bias (0.002 typical)
    float shadowMapSize;           // f32 — texture dimension (1024)
    float shadowStrength;          // f32 — min brightness in shadow (0.3 typical)
    // Projected light (up to 1 kFakeSpot with gobo texture)
    float projLightDir[4];        // vec4f — world-space direction (.w=0)
    float projLightColor[4];      // vec4f — RGB color (.a=1)
    float projLightProjRow0[4];   // vec4f — projection row 0: u = dot(worldPos, xyz) + w
    float projLightProjRow1[4];   // vec4f — projection row 1: v = dot(worldPos, xyz) + w
    float numProjLights;          // f32 — 0.0 or 1.0
    float _padProj[3];
};
static_assert(sizeof(SceneUniforms) == 656, "SceneUniforms must match WGSL layout");

struct MaterialUniforms {
    float color[4];             // vec4f
    float alphaThreshold;       // f32
    float useTexture;           // f32
    float specularPower;        // f32
    float emissiveMultiplier;   // f32
    float specularColor[4];     // vec4f
    float rimColor[4];          // vec4f — .rgb = color, .a = power
    float intensify;            // f32
    float shaderVariation;      // f32 — 0=none, 1=skin, 2=hair
    float rimLightUnder;        // f32 — 1.0 if rim only lights backfaces
    float deNormal;             // f32 — normal map diminish, 0=neutral
    float specular2Color[4];    // vec4f — .rgb = color, .a = power (2nd specular lobe)
    float anisotropy;           // f32
    float hasNormalMap;          // f32 — 1.0 when normal map bound
    float materialFogEnabled;   // f32 — 1.0 if fog applies to this material
    float prelit;               // f32 — 1.0 if vertex color is pre-lit
    float environMapStrength;   // f32 — 1.0 when environ map bound
    float environMapFalloff;    // f32 — 1.0 for Fresnel falloff
    float environMapSpecMask;   // f32 — 1.0 to mask by specular map alpha
    float texGenMode;           // f32 — 0=none, 1=xfm, 2=sphere, 3=projected, 4=xfmOrigin, 5=environ
    float texXfmRow0[4];        // vec4f — UV transform row 0 (u)
    float texXfmRow1[4];        // vec4f — UV transform row 1 (v)
    float normDetailTiling;     // f32 — UV tiling for detail normal map
    float normDetailStrength;   // f32 — blend strength (0 = disabled)
    float hasNormDetailMap;     // f32 — 1.0 when detail map bound
    float useAlphaAsRGB;        // f32 — 1.0 to use texture alpha as grayscale RGB (font textures)
    float hasSpecularMap;       // f32 — 1.0 when specular map bound
    float _padMat[3];           // pad to 16-byte boundary
};
static_assert(sizeof(MaterialUniforms) == 192, "MaterialUniforms must match WGSL layout");

struct ObjectUniforms {
    float world[16];            // mat4x4f
    float worldInvTranspose[16]; // mat4x4f
};
static_assert(sizeof(ObjectUniforms) == 128, "ObjectUniforms must match WGSL layout");

// Max bones per mesh (from Mesh.h MaxBones())
static constexpr int kMaxBones = 40;

struct BoneUniforms {
    float bones[kMaxBones][16]; // array<mat4x4f, 40>
};
static_assert(sizeof(BoneUniforms) == 2560, "BoneUniforms must match WGSL layout");
