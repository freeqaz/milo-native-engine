R"WGSL(
// DC3 Native — Standard Material Shader
// This file mirrors the Xbox-era material feature set, but is written to be
// readable as authored WGSL rather than as a direct extraction artifact.
// Supports: diffuse texture, normal mapping, detail normals, emissive/spec/rim
// maps, directional + point lights, alpha test, fog, environment maps, skin,
// hair, texgen, and skinned meshes.

const kShaderVariationNone = 0.0;
const kShaderVariationSkin = 1.0;
const kShaderVariationHair = 2.0;
const kShaderVariationWorldProjection = 3.0;

const kBoolThreshold = 0.5;
const kTexGenNone = 0.0;
const kTexGenXfm = 1.0;
const kTexGenSphere = 2.0;
const kTexGenProjected = 3.0;
const kTexGenXfmOrigin = 4.0;
const kTexGenEnviron = 5.0;
const kHighlightCompressionKnee = 0.9;
const kAlphaFringeFadeStart = 0.1;
// True for a pipeline whose blend ignores source alpha AND whose identity is
// not black -- Multiply (dst * src.rgb) and Darken (min(src, dst)) -- set per
// pipeline by PipelineManager::CreatePipeline (BlendSkipsFringeFade). The
// alpha fringe fade (fs_main, near the end) then leaves the colour alone:
// fading toward black would turn a white, alpha-0 Multiply quad into a full
// darken, which the image never draws (its NgMat Multiply is src Zero / dst
// SrcColor and its Darken One/One Min, neither reads alpha).
override kSkipFringeFade: bool = false;
// Soft-clip ("GX saturate") of the per-channel LIGHTING SUM (ambient + Σ light
// diffuse) before it modulates the base/texture colour. On Wii the rasterised
// channel colour is clamped to [0,1] *before* the TEV texture multiply, so
// lighting can tint a surface but can never push the output above the texture's
// own brightness — the venue's authored red/pink "hot" stage moments read as a
// saturated room, never a white-out. Our native lighting sum is UNBOUNDED: a
// close-up shot inside 4+4 point/dir light ranges (or the non-venue white-flood
// fallback, ambient 0.45 + 1.0 dir = 1.45×) reaches >1, and the downstream
// compressHighlights shoulder then maps any pale texture × that sum to ~1.0 → a
// textureless white flood (the small_club "pink wash" blowout). We restore the
// GX ceiling as a soft Reinhard rolloff rather than a hard min(.,1.0): IDENTITY
// for the entire GX-legal range (sum ≤ kLightSumKnee = 1.0) so nothing
// correctly-lit (incl. the whole diffuse N·L falloff up to N·L≈0.55) is dimmed,
// then a smooth asymptotic rolloff toward kLightSumCeiling for the over-drive
// pile-up (slope-1 at the knee → C1-continuous → no banding). Only the brightest
// over-1 highlights lose a little headroom; the runaway white flood is tamed.
const kLightSumKnee = 1.0;
const kLightSumCeiling = 1.05;
// Fraction of baked per-vertex chroma kept on non-prelit SKINNED (character)
// meshes when their skin/cloth textures are absent. 0 = fully desaturated (grey
// shading, no colour cast), 1 = raw vertex colour (re-casts the crowd purple).
// A low value keeps characters shaded (not flat white) without re-magenta-ing
// the crowd. See fs_main vertex-tint gate.
const kSkinnedVtxChroma = 0.25;

// === Bind Group 0: Per-scene (camera, environment) ===
struct SceneUniforms {
    viewProj: mat4x4f,
    view: mat4x4f,
    cameraPos: vec3f,
    _pad0: f32,
    // Fog
    fogColor: vec3f,
    fogStart: f32,
    fogEnd: f32,
    fogEnabled: f32,
    _pad1: vec2f,
    // Multi-light (up to 4 directional lights)
    lightDirs: array<vec4f, 4>,
    lightColors: array<vec4f, 4>,
    // Ambient
    ambientColor: vec4f,
    numLights: f32,
    _padN1: f32,
    _padN2: f32,
    _padN3: f32,
    // Point lights (up to 4)
    pointLightPos: array<vec4f, 4>,
    pointLightColors: array<vec4f, 4>,
    pointLightRanges: vec4f,
    numPointLights: f32,
    // Point-light distance-falloff law selector. 0 (default) = the legacy
    // saturate(1 - d/range)^2 hard cutoff (extinct at d>=range) — what DC3 and
    // every non-venue path use, kept byte-identical. 1 = the GX-faithful
    // inverse-linear law 1/(1 + d/range) (half-bright at d==range, long tail),
    // matching Wii GXInitLightAttn(k0=1,k1=1/range,k2=0) — set ONLY by RB3's
    // venue-light upload so arena range-55 spots reach the band 70-100u away.
    pointFalloffMode: f32,
    venueHighlightLumaMode: f32,
    outputLinear: f32,
    // Shadow mapping
    lightViewProj: mat4x4f,
    shadowEnabled: f32,
    shadowBias: f32,
    shadowMapSize: f32,
    shadowStrength: f32,
    // Projected light (1 kFakeSpot with gobo texture)
    projLightDir: vec4f,
    projLightColor: vec4f,
    projLightProjRow0: vec4f,   // u = dot(worldPos, xyz) + w
    projLightProjRow1: vec4f,   // v = dot(worldPos, xyz) + w
    numProjLights: f32,
    // 1.0: RB3 retail light model (see UniformStructs.h). 0: existing model.
    retailLighting: f32,
    // RB3 retail only (UniformStructs.h): the environ's AO strength when its
    // AO term is on (else 0), and its colour adjustment.
    aoStrength: f32,
    colorAdjust: f32,
    colorXfm: array<vec4f, 3>,
};

@group(0) @binding(0) var<uniform> scene: SceneUniforms;
@group(0) @binding(1) var shadowMap: texture_depth_2d;
@group(0) @binding(2) var shadowSampler: sampler_comparison;
@group(0) @binding(3) var projLightTex: texture_2d<f32>;
@group(0) @binding(4) var projLightSampler: sampler;

// === Bind Group 1: Per-material ===
struct MaterialUniforms {
    color: vec4f,
    alphaThreshold: f32,
    useTexture: f32,
    specularPower: f32,
    emissiveMultiplier: f32,
    specularColor: vec4f,
    rimColor: vec4f,        // .rgb = color, .a = power
    intensify: f32,
    shaderVariation: f32,   // 0=none, 1=skin, 2=hair
    rimLightUnder: f32,     // 1.0 = rim only on backlit edges
    deNormal: f32,          // normal map diminish (0=neutral, 1=no bumps, -1=exaggerate)
    specular2Color: vec4f,  // .rgb = color, .a = power (2nd specular lobe)
    anisotropy: f32,
    hasNormalMap: f32,       // 1.0 when normal map is bound
    materialFogEnabled: f32, // 1.0 if fog applies to this material
    prelit: f32,             // 1.0 if vertex color is pre-lit (skip lighting)
    environMapStrength: f32, // 1.0 when environ map is bound
    environMapFalloff: f32,  // 1.0 for Fresnel falloff at grazing angles
    environMapSpecMask: f32, // 1.0 to mask reflection by specular map alpha
    texGenMode: f32,         // 0=none, 1=xfm, 2=sphere, 3=projected, 4=xfmOrigin, 5=environ
    texXfmRow0: vec4f,       // UV transform row 0
    texXfmRow1: vec4f,       // UV transform row 1
    normDetailTiling: f32,   // UV tiling for detail normal map
    normDetailStrength: f32, // blend strength (0 = disabled)
    hasNormDetailMap: f32,   // 1.0 when detail map bound
    useAlphaAsRGB: f32,      // 1.0 to use texture alpha as grayscale RGB (font textures)
    hasSpecularMap: f32,     // 1.0 when specular map is bound
    unlit: f32,              // 1.0 = Wii mUseEnviron==0 && !mPreLit: register color only (no ambient/lights/vertex tint)
    bloomMaskScale: f32,     // >0: alpha = luma(rgb) * this (RB3 pseudo-HDR bloom mask)
    gammaShading: f32,       // 1.0: shade in gamma space, texel * colour * lighting as stored, no encode (RB3 retail)
    // RB3 retail material terms (UniformStructs.h), read under scene.retailLighting.
    retailSpec: vec4f,       // .rgb specular colour (0 = none), .a power
    retailRim: vec4f,        // .rgb rim colour (0 = none), .a power
    retailFlags: vec4f,      // x per-pixel lit, y normal map, z specular map, w rim map
    // RB3 colour modulation (UniformStructs.h): xyz colours as RGBA8, w mode
    // in the low 16 bits; the high 16 bits of w carry the world refraction
    // strength as an f16 (0 = no refraction, see refractStrength).
    colorMod: vec4u,
};

@group(1) @binding(0) var<uniform> material: MaterialUniforms;
@group(1) @binding(1) var diffuseTex: texture_2d<f32>;
@group(1) @binding(2) var diffuseSampler: sampler;
@group(1) @binding(3) var normalMapTex: texture_2d<f32>;
@group(1) @binding(4) var specularMapTex: texture_2d<f32>;
@group(1) @binding(5) var emissiveMapTex: texture_2d<f32>;
@group(1) @binding(6) var rimMapTex: texture_2d<f32>;
@group(1) @binding(7) var mapSampler: sampler;
@group(1) @binding(8) var environMapTex: texture_cube<f32>;
@group(1) @binding(9) var environSampler: sampler;
@group(1) @binding(10) var normDetailMapTex: texture_2d<f32>;
// Retail world refraction (standard.ps option bit 46, mRefractWorld): the
// refract normal map (retail tf1), the frame being refracted (retail tf6,
// DxRnd::GetCurrentFrameTex: the pre-process buffer SavePreBuffer fills at
// world end, or the post buffer once post-processing has run) and its
// linear clamp sampler.
@group(1) @binding(11) var refractNormalTex: texture_2d<f32>;
@group(1) @binding(12) var screenTex: texture_2d<f32>;
@group(1) @binding(13) var screenSampler: sampler;

// === Bind Group 2: Per-object ===
struct ObjectUniforms {
    world: mat4x4f,
    worldInvTranspose: mat4x4f,
    // RB3 retail box map for this draw, faces {+X,-X,+Y,-Y,+Z,-Z} (retail
    // c80..c85); read only under scene.retailLighting.
    boxLight: array<vec4f, 6>,
    // RB3 retail: x = mesh has baked AO, y = approx lights queued for this draw.
    retail: vec4f,
};

@group(2) @binding(0) var<uniform> object: ObjectUniforms;

// === Bind Group 3: Per-draw bone palette (skinned meshes) ===
@group(3) @binding(0) var<uniform> bones: array<mat4x4f, 40>;

// === Vertex/Fragment IO ===
struct VertexInput {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) color: vec4f,
    @location(3) uv: vec2f,
    @location(4) tangent: vec4f,
};

struct SkinnedVertexInput {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) color: vec4f,
    @location(3) uv: vec2f,
    @location(4) boneWeights: vec4f,
    @location(5) boneIndices: vec4u,
    @location(6) tangent: vec4f,
};

struct VertexOutput {
    @builtin(position) clipPos: vec4f,
    @location(0) worldPos: vec3f,
    @location(1) worldNormal: vec3f,
    @location(2) color: vec4f,
    @location(3) uv: vec2f,
    @location(4) worldTangent: vec3f,
    @location(5) worldBitangent: vec3f,
    // 1.0 for skinned (character) meshes, 0.0 for static. Lets fs_main keep the
    // baked per-vertex colour on band/crowd characters — whose vertex colour is
    // their entire visible skin/cloth tint when textures are absent — while still
    // suppressing it on non-prelit STATIC venue geometry (whose baked cool/purple
    // vertex lighting double-applied with the scene light, washing it magenta).
    // Set per vertex entry point (vs_main=0.0 / vs_skinned=1.0); flat-interpolated
    // since it is constant across a primitive.
    @location(6) @interpolate(flat) isSkinned: f32,
    // RB3 retail light model (scene.retailLighting): the box map + point
    // lights evaluated at the vertex, as standard.vs does for a material
    // without per_pixel_lit.
    @location(7) vertexLit: vec3f,
    // RB3 retail ambient occlusion factors (retailAOFactors): x ambient and
    // box map, y and z the two point lights, w 1 when the AO term is on.
    @location(8) retailAO: vec4f,
    // RB3 retail specular evaluated per vertex (a material without
    // per_pixel_lit), added after the texel multiply.
    @location(9) vertexSpec: vec3f,
};

struct LightingTerms {
    diffuse: vec3f,
    specular: vec3f,
};

fn isEnabled(value: f32) -> bool {
    return value > kBoolThreshold;
}

// RB3 material colour modulation (RndMat mColorModFlags / mColorMod; the mode
// is material.colorMod.w, the colours its x, y, z as RGBA8). The math is retail's xbox_shaders: the diffuse
// texel's alpha `a` selects a band and a position t in it, and the shaded
// colour is scaled by mix(colour[band], 1, t). Membership is
// clamp(a, lo, hi) == a, so a value on a band edge belongs to both bands and
// both factors apply (the shader chains three conditional multiplies).
//   3 Modulate:            bands [0, 1/3], [1/3, 2/3], [2/3, 1]; t = 3a - band
//                          (DC3 permutations with key bits 32-33 = 3)
//   2 AlphaUnpackModulate: alpha packed into [0.1, 1]: bands [0.1, 0.4],
//                          [0.4, 0.7], [0.7, 1], t = (a - lo) / 0.3, and the
//                          output alpha is 10a (RB3 permutation 0x4202000010)
//   1 AlphaPack:           output alpha 0.1 + 0.9a, colour unchanged
fn colorModMode() -> u32 {
    return material.colorMod.w & 0xFFu;
}

// Bit 8 of colorMod.w: the shape says this material's texel alpha selects a
// colour-mod band instead of carrying coverage (rndshape::
// ColorModAlphaIsBandSelector), so the alpha fringe fade must not read it.
// Never set by the RB3-Wii shape.
fn colorModAlphaIsBandSelector() -> bool {
    return ((material.colorMod.w >> 8u) & 1u) != 0u;
}

// RndMat::GetRefractStrength for a material whose refraction is on
// (GetRefractEnabled: enabled, strength > 0 and a refract normal map), else 0.
fn refractStrength() -> f32 {
    return unpack2x16float(material.colorMod.w).y;
}

// sRGB EOTF (sRGB -> linear), the inverse of linearToSrgb below.
fn srgbToLinear(color: vec3f) -> vec3f {
    let lo = color / 12.92;
    let hi = pow((max(color, vec3f(0.0)) + 0.055) / 1.055, vec3f(2.4));
    return select(lo, hi, color > vec3f(0.04045));
}

// Retail world refraction, standard.ps with option bit 46 (e.g. permutation
// standard_0000400000000000): the refract normal map at the material uv gives
// n = (g * 2 - 1, r * 2 - 1); the frame is fetched at the clip position pushed
// by n * strength (c119.w, kPS_RefractStrength) before the divide,
//   uv = (clip.xy + n * strength) * (0.5, -0.5) / clip.w + 0.5,
// rgb only (alpha is the texel's). The caller multiplies it into the texel.
// The frame holds output values (8-bit, display-encoded); a gamma-space
// material uses them as stored, a linear one decodes them first.
fn refractScreen(worldPos: vec3f, uv: vec2f, strength: f32, gammaShading: bool) -> vec3f {
    let c = scene.viewProj * vec4f(worldPos, 1.0);
    let ns = textureSample(refractNormalTex, diffuseSampler, uv);
    let n = vec2f(ns.g * 2.0 - 1.0, ns.r * 2.0 - 1.0);
    let suv = (c.xy + n * strength) * vec2f(0.5, -0.5) / c.w + vec2f(0.5);
    let screen = textureSampleLevel(screenTex, screenSampler, suv, 0.0).rgb;
    if (gammaShading) {
        return screen;
    }
    return srgbToLinear(screen);
}

fn colorModBand(packed: u32, a: f32, lo: f32, hi: f32) -> vec3f {
    if (clamp(a, lo, hi) != a) {
        return vec3f(1.0);
    }
    return mix(unpack4x8unorm(packed).rgb, vec3f(1.0), (a - lo) / (hi - lo));
}

fn colorModScale(a: f32) -> vec3f {
    let mode = colorModMode();
    let c = material.colorMod;
    if (mode == 3u) {
        return colorModBand(c.x, a, 0.0, 1.0 / 3.0) * colorModBand(c.y, a, 1.0 / 3.0, 2.0 / 3.0) *
               colorModBand(c.z, a, 2.0 / 3.0, 1.0);
    }
    if (mode == 2u) {
        return colorModBand(c.x, a, 0.1, 0.4) * colorModBand(c.y, a, 0.4, 0.7) *
               colorModBand(c.z, a, 0.7, 1.0);
    }
    return vec3f(1.0);
}

// The factor the texel alpha contributes to the output alpha.
fn colorModAlpha(a: f32) -> f32 {
    let mode = colorModMode();
    if (mode == 1u) {
        return 0.1 + 0.9 * a;
    }
    if (mode == 2u) {
        return saturate(10.0 * a);
    }
    return a;
}

fn shaderVariationIs(value: f32, expectedVariation: f32) -> bool {
    return value > expectedVariation - kBoolThreshold && value < expectedVariation + kBoolThreshold;
}

fn texGenModeIs(value: f32, expectedMode: f32) -> bool {
    return value > expectedMode - kBoolThreshold && value < expectedMode + kBoolThreshold;
}

fn boneWeightAt(weights: vec4f, influenceIndex: u32) -> f32 {
    switch (influenceIndex) {
        case 0u: { return weights.x; }
        case 1u: { return weights.y; }
        case 2u: { return weights.z; }
        default: { return weights.w; }
    }
}

fn boneIndexAt(indices: vec4u, influenceIndex: u32) -> u32 {
    switch (influenceIndex) {
        case 0u: { return indices.x; }
        case 1u: { return indices.y; }
        case 2u: { return indices.z; }
        default: { return indices.w; }
    }
}

fn normalizeBoneWeights(rawWeights: vec4f) -> vec4f {
    let totalWeight = rawWeights.x + rawWeights.y + rawWeights.z + rawWeights.w;
    if (totalWeight > 0.0) {
        return rawWeights / totalWeight;
    }

    // UDEC4N bone weights can decode to all zeros on bad/missing data. Preserve
    // the original fallback of binding the vertex entirely to the first bone.
    return vec4f(1.0, 0.0, 0.0, 0.0);
}

// === TexGen UV computation ===
fn computeTexGenUV(baseUV: vec2f, worldPos: vec3f, worldNormal: vec3f, viewDir: vec3f) -> vec2f {
    let texGenMode = material.texGenMode;
    if (texGenModeIs(texGenMode, kTexGenNone)) {
        return baseUV;
    }

    if (texGenModeIs(texGenMode, kTexGenXfm)) {
        // kTexGenXfm: transform UV about the 0.5-centered texture origin.
        let centeredUV = baseUV - vec2f(0.5);
        return vec2f(
            dot(centeredUV, material.texXfmRow0.xy) + material.texXfmRow0.z + 0.5,
            dot(centeredUV, material.texXfmRow1.xy) + material.texXfmRow1.z + 0.5
        );
    }

    if (texGenModeIs(texGenMode, kTexGenSphere)) {
        // kTexGenSphere: classic view-space sphere map from the world normal.
        let viewSpaceNormal = (scene.view * vec4f(worldNormal, 0.0)).xyz;
        return viewSpaceNormal.xy * 0.5 + 0.5;
    }

    if (texGenModeIs(texGenMode, kTexGenProjected)) {
        // kTexGenProjected: project world position onto the stored transform rows.
        return vec2f(
            dot(worldPos, material.texXfmRow0.xyz) + material.texXfmRow0.w,
            dot(worldPos, material.texXfmRow1.xyz) + material.texXfmRow1.w
        );
    }

    if (texGenModeIs(texGenMode, kTexGenXfmOrigin)) {
        // kTexGenXfmOrigin: transform UVs without re-centering them around 0.5.
        return vec2f(
            dot(baseUV, material.texXfmRow0.xy) + material.texXfmRow0.z,
            dot(baseUV, material.texXfmRow1.xy) + material.texXfmRow1.z
        );
    }

    // kTexGenEnviron: reflection-based UVs for legacy environment-mapped materials.
    let reflectionDir = reflect(-viewDir, worldNormal);
    let viewSpaceReflection = (scene.view * vec4f(reflectionDir, 0.0)).xyz;
    return viewSpaceReflection.xy * 0.5 + 0.5;
}

fn decodeTangentSpaceNormal(normalSample: vec4f) -> vec3f {
    // Xbox 360 assets frequently use DXT5nm (X in alpha, Y in green). If R and B
    // are basically empty, treat it as that encoding; otherwise use plain RGB XYZ.
    if (normalSample.r + normalSample.b < 0.1) {
        let tangentX = normalSample.a * 2.0 - 1.0;
        let tangentY = normalSample.g * 2.0 - 1.0;
        let tangentZ = sqrt(max(1.0 - tangentX * tangentX - tangentY * tangentY, 0.0));

        if (normalSample.a < 0.01 && normalSample.g < 0.01) {
            return vec3f(0.0, 0.0, 1.0);
        }

        return vec3f(tangentX, tangentY, tangentZ);
    }

    return normalSample.rgb * 2.0 - 1.0;
}

fn applyDeNormal(tangentSpaceNormal: vec3f) -> vec3f {
    let diminishAmount = clamp(material.deNormal, -3.0, 1.0);
    return normalize(vec3f(
        tangentSpaceNormal.x * (1.0 - diminishAmount),
        tangentSpaceNormal.y * (1.0 - diminishAmount),
        mix(tangentSpaceNormal.z, 1.0, max(diminishAmount, 0.0))
    ));
}

fn blendDetailNormal(baseNormal: vec3f, uv: vec2f) -> vec3f {
    if (!isEnabled(material.hasNormDetailMap)) {
        return baseNormal;
    }

    let detailUV = uv * material.normDetailTiling;
    let detailSample = textureSample(normDetailMapTex, mapSampler, detailUV);
    let detailNormal = detailSample.xyz * 2.0 - 1.0;
    return normalize(vec3f(
        baseNormal.x + detailNormal.x * material.normDetailStrength,
        baseNormal.y + detailNormal.y * material.normDetailStrength,
        baseNormal.z
    ));
}

fn sampleSurfaceNormal(surfaceInput: VertexOutput) -> vec3f {
    var worldNormal = normalize(surfaceInput.worldNormal);
    if (!isEnabled(material.hasNormalMap)) {
        return worldNormal;
    }

    let normalSample = textureSample(normalMapTex, mapSampler, surfaceInput.uv);
    var tangentSpaceNormal = decodeTangentSpaceNormal(normalSample);
    tangentSpaceNormal = applyDeNormal(tangentSpaceNormal);
    tangentSpaceNormal = blendDetailNormal(tangentSpaceNormal, surfaceInput.uv);

    let worldTangent = normalize(surfaceInput.worldTangent);
    let worldBitangent = normalize(surfaceInput.worldBitangent);
    worldNormal = normalize(
        worldTangent * tangentSpaceNormal.x +
        worldBitangent * tangentSpaceNormal.y +
        worldNormal * tangentSpaceNormal.z
    );

    return worldNormal;
}

fn emptyLightingTerms() -> LightingTerms {
    return LightingTerms(vec3f(0.0), vec3f(0.0));
}

fn pointLightRangeAt(lightIndex: i32) -> f32 {
    switch (lightIndex) {
        case 0: { return scene.pointLightRanges.x; }
        case 1: { return scene.pointLightRanges.y; }
        case 2: { return scene.pointLightRanges.z; }
        default: { return scene.pointLightRanges.w; }
    }
}

fn sampleShadowFactor(worldPos: vec3f) -> f32 {
    let shadowClipPos = scene.lightViewProj * vec4f(worldPos, 1.0);
    let shadowNdc = shadowClipPos.xyz / shadowClipPos.w;
    let unclampedShadowUV = shadowNdc.xy * vec2f(0.5, -0.5) + 0.5;
    let clampedShadowUV = clamp(unclampedShadowUV, vec2f(0.0), vec2f(1.0));
    let compareDepth = clamp(shadowNdc.z - scene.shadowBias, 0.0, 1.0);
    let texelSize = 1.0 / max(scene.shadowMapSize, 1.0);

    // textureSampleCompare must stay in uniform control flow, so always sample.
    var shadowVisibility = 0.0;
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(-texelSize, -texelSize), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(0.0, -texelSize), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(texelSize, -texelSize), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(-texelSize, 0.0), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV, compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(texelSize, 0.0), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(-texelSize, texelSize), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(0.0, texelSize), compareDepth);
    shadowVisibility += textureSampleCompare(shadowMap, shadowSampler, clampedShadowUV + vec2f(texelSize, texelSize), compareDepth);
    shadowVisibility /= 9.0;

    let shadowLookupValid = isEnabled(scene.shadowEnabled)
        && unclampedShadowUV.x >= 0.0 && unclampedShadowUV.x <= 1.0
        && unclampedShadowUV.y >= 0.0 && unclampedShadowUV.y <= 1.0
        && shadowNdc.z >= 0.0 && shadowNdc.z <= 1.0;
    if (shadowLookupValid) {
        return mix(scene.shadowStrength, 1.0, shadowVisibility);
    }

    return 1.0;
}

fn computeStandardLight(
    surfaceNormal: vec3f,
    viewDir: vec3f,
    lightDir: vec3f,
    lightColor: vec3f,
    specularSample: vec4f
) -> LightingTerms {
    var lighting = emptyLightingTerms();
    let lambert = max(dot(surfaceNormal, lightDir), 0.0);
    lighting.diffuse = lightColor * lambert;

    if (material.specularPower > 0.0 && lambert > 0.0) {
        let halfVector = normalize(lightDir + viewDir);
        if (isEnabled(material.hasSpecularMap)) {
            // Has specular map: texture modulates power with min 0.1 floor
            let glossExponent = max(material.specularPower * specularSample.a, 0.1);
            let specularStrength = pow(max(dot(surfaceNormal, halfVector), 0.0), glossExponent);
            lighting.specular = specularSample.rgb * lightColor * specularStrength;
        } else {
            // No specular map: material uniform directly
            let specularStrength = pow(max(dot(surfaceNormal, halfVector), 0.0), material.specularPower);
            lighting.specular = material.specularColor.rgb * lightColor * specularStrength;
        }
    }

    return lighting;
}

fn computeSkinLight(
    surfaceNormal: vec3f,
    viewDir: vec3f,
    lightDir: vec3f,
    lightColor: vec3f,
    specularSample: vec4f
) -> LightingTerms {
    var lighting = emptyLightingTerms();
    let rawNdotL = dot(surfaceNormal, lightDir);

    // Half-Lambert softens the terminator; the warm tint approximates cheap SSS.
    let wrappedDiffuse = rawNdotL * 0.5 + 0.5;
    let skinDiffuse = wrappedDiffuse * wrappedDiffuse;
    let shadowZone = smoothstep(0.0, 0.5, rawNdotL);
    let warmShadowTint = mix(vec3f(0.85, 0.45, 0.25), vec3f(1.0), shadowZone);
    lighting.diffuse = lightColor * skinDiffuse * warmShadowTint;

    let halfVector = normalize(lightDir + viewDir);
    let specularDot = max(dot(surfaceNormal, halfVector), 0.0);

    if (material.specularPower > 0.0) {
        if (isEnabled(material.hasSpecularMap)) {
            let primaryExponent = max(material.specularPower * specularSample.a, 0.1);
            let primaryLobe = pow(specularDot, primaryExponent);
            lighting.specular += specularSample.rgb * lightColor * primaryLobe;
        } else {
            let primaryLobe = pow(specularDot, material.specularPower);
            lighting.specular += material.specularColor.rgb * lightColor * primaryLobe;
        }
    }

    let secondaryExponent = material.specular2Color.a;
    if (secondaryExponent > 0.0) {
        let secondaryLobe = pow(specularDot, secondaryExponent);
        lighting.specular += material.specular2Color.rgb * lightColor * secondaryLobe;
    }

    return lighting;
}

fn computeHairLight(
    surfaceNormal: vec3f,
    viewDir: vec3f,
    lightDir: vec3f,
    lightColor: vec3f,
    worldTangent: vec3f,
    specularSample: vec4f
) -> LightingTerms {
    var lighting = emptyLightingTerms();
    let wrappedDiffuse = dot(surfaceNormal, lightDir) * 0.5 + 0.5;
    lighting.diffuse = lightColor * wrappedDiffuse;

    let strandTangent = normalize(worldTangent);
    let halfVector = normalize(lightDir + viewDir);
    let tangentDotHalf = dot(strandTangent, halfVector);
    let sineTerm = sqrt(max(1.0 - tangentDotHalf * tangentDotHalf, 0.0));
    let anisotropicExponent = max(material.anisotropy, 1.0);
    let anisotropicSpecular = pow(sineTerm, anisotropicExponent);
    if (isEnabled(material.hasSpecularMap)) {
        lighting.specular = specularSample.rgb * lightColor * anisotropicSpecular;
    } else {
        lighting.specular = material.specularColor.rgb * lightColor * anisotropicSpecular;
    }

    return lighting;
}

fn computeDirectionalLight(
    surfaceNormal: vec3f,
    viewDir: vec3f,
    lightDir: vec3f,
    lightColor: vec3f,
    worldTangent: vec3f,
    specularSample: vec4f,
    isSkinMaterial: bool,
    isHairMaterial: bool
) -> LightingTerms {
    if (isHairMaterial) {
        return computeHairLight(surfaceNormal, viewDir, lightDir, lightColor, worldTangent, specularSample);
    }

    if (isSkinMaterial) {
        return computeSkinLight(surfaceNormal, viewDir, lightDir, lightColor, specularSample);
    }

    return computeStandardLight(surfaceNormal, viewDir, lightDir, lightColor, specularSample);
}

fn computePointLight(
    surfaceNormal: vec3f,
    viewDir: vec3f,
    worldPos: vec3f,
    lightIndex: i32,
    specularSample: vec4f
) -> LightingTerms {
    var lighting = emptyLightingTerms();
    let lightPos = scene.pointLightPos[lightIndex].xyz;
    let toLight = lightPos - worldPos;
    let lightDistance = length(toLight);
    let lightDir = toLight / max(lightDistance, 0.001);

    let lightRange = pointLightRangeAt(lightIndex);
    var falloff: f32;
    if (scene.pointFalloffMode >= 1.5) {
        // RB3 retail (NgEnviron SetPointLightRegisters + standard.vs): the
        // position's .w carries 1/(falloffStart - range) and the colour's .w
        // -range/(falloffStart - range), so the light is full strength inside
        // falloffStart and fades linearly to zero at range. Both are 0 and 1
        // when falloffStart >= range: no falloff at all.
        falloff = saturate(lightDistance * scene.pointLightPos[lightIndex].w
            + scene.pointLightColors[lightIndex].w);
    } else if (scene.pointFalloffMode >= 0.5) {
        // GX-faithful inverse-linear falloff: GXInitLightAttn(.., k0=1, k1=1/range,
        // k2=0) => 1/(1 + d/range). Half-bright at d==range, long tail (matches Wii
        // Lit.cpp:36-44) — not a hard cutoff. Set only by the RB3 venue-light path.
        falloff = 1.0 / (1.0 + lightDistance / max(lightRange, 0.001));
    } else {
        // Legacy default (DC3 + every non-venue path): squared-saturate hard
        // cutoff, exactly 0 for d >= range. Byte-identical to pre-change behaviour.
        let rangeAttenuation = saturate(1.0 - lightDistance / max(lightRange, 0.001));
        falloff = rangeAttenuation * rangeAttenuation;
    }
    let lightColor = scene.pointLightColors[lightIndex].rgb * falloff;

    let lambert = max(dot(surfaceNormal, lightDir), 0.0);
    lighting.diffuse = lightColor * lambert;

    if (material.specularPower > 0.0 && lambert > 0.0) {
        let halfVector = normalize(lightDir + viewDir);
        if (isEnabled(material.hasSpecularMap)) {
            let glossExponent = max(material.specularPower * specularSample.a, 0.1);
            let specularStrength = pow(max(dot(surfaceNormal, halfVector), 0.0), glossExponent);
            lighting.specular = specularSample.rgb * lightColor * specularStrength;
        } else {
            let specularStrength = pow(max(dot(surfaceNormal, halfVector), 0.0), material.specularPower);
            lighting.specular = material.specularColor.rgb * lightColor * specularStrength;
        }
    }

    return lighting;
}

fn computeRimLighting(surfaceNormal: vec3f, viewDir: vec3f, uv: vec2f) -> vec3f {
    let rimPower = max(material.rimColor.a, 0.5);
    if (rimPower <= 0.0 || length(material.rimColor.rgb) <= 0.0) {
        return vec3f(0.0);
    }

    let rimMapSample = textureSample(rimMapTex, mapSampler, uv);
    let rimDot = 1.0 - max(dot(surfaceNormal, viewDir), 0.0);
    let rimExponent = max(rimPower * rimMapSample.a, 0.5);
    var rimColor =
        pow(rimDot, rimExponent) *
        material.rimColor.rgb *
        rimMapSample.rgb *
        0.15;

    // Some materials only want rim light where the main light is behind the surface.
    if (isEnabled(material.rimLightUnder) && scene.numLights > 0.0) {
        let primaryLightDir = normalize(-scene.lightDirs[0].xyz);
        let backlightMask = saturate(1.0 - dot(surfaceNormal, primaryLightDir));
        rimColor *= backlightMask;
    }

    return rimColor;
}

fn computeEnvironmentReflection(surfaceNormal: vec3f, viewDir: vec3f, specularSample: vec4f) -> vec3f {
    if (!isEnabled(material.environMapStrength)) {
        return vec3f(0.0);
    }

    let reflectionDir = reflect(-viewDir, surfaceNormal);
    let environmentColor = textureSample(environMapTex, environSampler, reflectionDir).rgb;

    var fresnelFactor = 1.0;
    if (isEnabled(material.environMapFalloff)) {
        let viewDotNormal = max(dot(surfaceNormal, viewDir), 0.0);
        fresnelFactor = 1.0 - viewDotNormal;
        fresnelFactor *= fresnelFactor;
    }

    var reflectionMask = 1.0;
    if (isEnabled(material.environMapSpecMask)) {
        reflectionMask = specularSample.a;
    }

    return environmentColor * fresnelFactor * reflectionMask;
}

// Per-channel soft saturate of the lighting sum (see kLightSumKnee comment).
// Identity below the knee; smooth Reinhard rolloff toward the ceiling above it.
// over/(over+span) has slope 1 at over=0 (C1-continuous at the knee) and
// asymptotes to 1 as over→∞, so the output asymptotes to `ceiling`.
fn softClipLighting(lightSum: vec3f) -> vec3f {
    let knee = vec3f(kLightSumKnee);
    let span = vec3f(kLightSumCeiling - kLightSumKnee);
    let over = max(lightSum - knee, vec3f(0.0));
    let rolled = knee + span * (over / (over + span));
    return select(lightSum, rolled, lightSum > knee);
}

// RB3 retail box map (standard.vs, constants c80..c85): each face colour is
// weighted by the positive part of the unit normal along that axis.
fn boxLightSample(N: vec3f) -> vec3f {
    var c = vec3f(0.0);
    c += max(0.0,  N.x) * object.boxLight[0].rgb;
    c += max(0.0, -N.x) * object.boxLight[1].rgb;
    c += max(0.0,  N.y) * object.boxLight[2].rgb;
    c += max(0.0, -N.y) * object.boxLight[3].rgb;
    c += max(0.0,  N.z) * object.boxLight[4].rgb;
    c += max(0.0, -N.z) * object.boxLight[5].rgb;
    return c;
}

// Whether the draw has any light at all: retail's CalcShaderOpts gates the
// lighting terms (box map, points, specular, per-pixel, rim) on real or
// approx lights being present.
fn retailLightsOn() -> bool {
    return object.retail.y > 0.5 || scene.numPointLights > 0.5;
}

// RB3 retail point light i at a position: colour times retail's linear
// falloff, and the unit direction to the light.
fn retailPointColor(i: i32, worldPos: vec3f) -> vec3f {
    let d = length(scene.pointLightPos[i].xyz - worldPos);
    let atten = saturate(d * scene.pointLightPos[i].w + scene.pointLightColors[i].w);
    return scene.pointLightColors[i].rgb * atten;
}
fn retailPointDir(i: i32, worldPos: vec3f) -> vec3f {
    let toLight = scene.pointLightPos[i].xyz - worldPos;
    return toLight / max(length(toLight), 0.001);
}
fn retailPointAO(ao: vec4f, i: i32) -> f32 {
    if (i == 0) { return ao.y; }
    if (i == 1) { return ao.z; }
    return 1.0;
}

// RB3 retail ambient occlusion (standard.vs, option bit 38: not prelit, the
// mesh HasAOCalc, the environ's AO on with strength S > 0.003). The vertex
// colour holds the baked visibility as four spherical-harmonic coefficients
// (RndAmbientOcclusion::CalculateAOAtPoint): r the constant band, a/g/b the
// x/y/z linear bands remapped to [0, 1]. Ambient and box light are scaled by
// sat((1.128379 r - 1) S + 1); each point light by sat((v - 1) S + 1), v the
// visibility toward the light over a cosine lobe, with the light direction
// taken into the mesh's local frame.
fn retailAOFactors(worldPos: vec3f, localNormal: vec3f, sh: vec4f) -> vec4f {
    if (scene.retailLighting < 0.5 || scene.aoStrength <= 0.0 || object.retail.x < 0.5 ||
        material.prelit > 0.5) {
        return vec4f(1.0, 1.0, 1.0, 0.0);
    }
    let S = scene.aoStrength;
    let amb = saturate((1.128379 * sh.r - 1.0) * S + 1.0);
    var pt = vec2f(1.0);
    let M = mat3x3f(object.world[0].xyz, object.world[1].xyz, object.world[2].xyz);
    let linearBands = vec3f(sh.a, sh.g, sh.b) * 2.0 - 1.0;
    let pointLightCount = i32(scene.numPointLights);
    for (var i = 0; i < 2; i++) {
        if (i >= pointLightCount) {
            break;
        }
        let lightLocal = retailPointDir(i, worldPos) * M;
        let lobe = 0.1875 + 0.5625 * dot(localNormal, lightLocal);
        var v = 1.0;
        if (lobe >= 0.0) {
            v = (0.2820948 * sh.r + 0.4886025 * dot(linearBands, lightLocal)) / max(lobe, 1e-6);
        }
        pt[i] = saturate((v - 1.0) * S + 1.0);
    }
    return vec4f(amb, pt.x, pt.y, 1.0);
}

// standard.vs / standard.ps diffuse lighting, ambient excluded: the box map
// plus the real point lights (Lambert, retail's linear falloff), each scaled
// by its AO factor.
fn retailDiffuseLight(worldPos: vec3f, N: vec3f, ao: vec4f) -> vec3f {
    var lit = ao.x * boxLightSample(N);
    let pointLightCount = i32(scene.numPointLights);
    for (var i = 0; i < 4; i++) {
        if (i >= pointLightCount) {
            break;
        }
        lit += retailPointAO(ao, i) * retailPointColor(i, worldPos) *
            max(dot(N, retailPointDir(i, worldPos)), 0.0);
    }
    return lit;
}

fn retailVertexLight(worldPos: vec3f, worldNormal: vec3f, ao: vec4f) -> vec3f {
    if (scene.retailLighting < 0.5) {
        return vec3f(0.0);
    }
    return retailDiffuseLight(worldPos, normalize(worldNormal), ao);
}

// RB3 retail specular (standard.vs without per_pixel_lit, standard.ps with
// it): an energy-normalised Phong lobe p/(2 pi) + 1/pi around the reflected
// view vector R. The box map contributes pow(sat(R . axis), p) per face,
// scaled by a Fresnel-like 0.25 + (1 - sat(N . V)) (0.5 N.z + 0.5) and the
// ambient AO factor; each point light contributes
// pow(sat(R . L), p) times its colour, falloff and AO factor (no Lambert
// term). The sum is tinted by the specular colour.
fn retailSpecular(worldPos: vec3f, N: vec3f, V: vec3f, ao: vec4f, power: f32) -> vec3f {
    let NdotV = dot(N, V);
    let lobeScale = power * 0.1591549 + 0.3183099;
    let fresnel = 0.25 + (1.0 - saturate(NdotV)) * (0.5 * N.z + 0.5);
    let R = 2.0 * NdotV * N - V;
    var box = vec3f(0.0);
    box += pow(saturate( R.x), power) * object.boxLight[0].rgb;
    box += pow(saturate(-R.x), power) * object.boxLight[1].rgb;
    box += pow(saturate( R.y), power) * object.boxLight[2].rgb;
    box += pow(saturate(-R.y), power) * object.boxLight[3].rgb;
    box += pow(saturate( R.z), power) * object.boxLight[4].rgb;
    box += pow(saturate(-R.z), power) * object.boxLight[5].rgb;
    var spec = lobeScale * fresnel * ao.x * box;
    let pointLightCount = i32(scene.numPointLights);
    for (var i = 0; i < 4; i++) {
        if (i >= pointLightCount) {
            break;
        }
        spec += lobeScale * retailPointAO(ao, i) * retailPointColor(i, worldPos) *
            pow(saturate(dot(R, retailPointDir(i, worldPos))), power);
    }
    return spec;
}

fn retailHasSpecular() -> bool {
    return dot(abs(material.retailSpec.rgb), vec3f(1.0)) > 0.0;
}

// DC3 (scene.retailLighting 2): the material uses the environ. The DC3
// shape's MatRetailTerms fills the retail terms only for such a material, and
// a filled set always carries a specular power of at least 0.5.
fn dc3UsesEnviron() -> bool {
    return material.retailSpec.a > 0.0;
}

// Specular evaluated at the vertex: a material with specular but without
// per_pixel_lit (option bit 2 without bit 0).
fn retailVertexSpecular(worldPos: vec3f, worldNormal: vec3f, ao: vec4f) -> vec3f {
    if (scene.retailLighting < 0.5 || isEnabled(material.retailFlags.x) || !retailHasSpecular() ||
        !retailLightsOn()) {
        return vec3f(0.0);
    }
    let N = normalize(worldNormal);
    let V = normalize(scene.cameraPos - worldPos);
    return material.retailSpec.rgb * retailSpecular(worldPos, N, V, ao, material.retailSpec.a);
}

fn compressHighlights(color: vec3f) -> vec3f {
    let knee = vec3f(kHighlightCompressionKnee);
    return select(
        color,
        knee + (vec3f(1.0) - knee) * tanh((color - knee) / (vec3f(1.0) - knee)),
        color > knee
    );
}

// WHITE-fix (Wave 9): luminance-preserving highlight compression for the VENUE path.
// Compress the LUMINANCE with the same tanh shoulder, then scale RGB uniformly so
// hue/saturation survive — a hot venue moment stays bright-but-colored instead of the
// per-channel path flattening every channel to 1.0 (white). Identity below the knee, so
// correctly-exposed venue frames are unchanged.
fn compressHighlightsLuma(color: vec3f) -> vec3f {
    let luma = dot(color, vec3f(0.2126, 0.7152, 0.0722));
    if (luma <= kHighlightCompressionKnee) { return color; }
    let k = kHighlightCompressionKnee;
    let rolled = k + (1.0 - k) * tanh((luma - k) / (1.0 - k));
    return color * (rolled / max(luma, 1e-4));
}

// sRGB OETF (linear -> sRGB). The web canvas surface is configured as a
// NON-sRGB format (RGBA8Unorm / BGRA8Unorm — caps.formats[0] on
// Chrome/ANGLE), so the browser presents whatever bytes we write WITHOUT
// applying the linear->display gamma encode. Our material/lighting math runs
// in a linear working space, so writing those linear values straight to a
// non-sRGB surface displays everything ~2.5x too dark (W9: scene avgLuma 34
// vs Wii reference ~90). Apply the sRGB transfer function at the fragment
// output so the displayed pixels carry the correct perceptual brightness, the
// same encode the GPU would do for free on an *_UnormSrgb surface.
fn linearToSrgb(color: vec3f) -> vec3f {
    let lo = color * 12.92;
    let hi = 1.055 * pow(max(color, vec3f(0.0)), vec3f(1.0 / 2.4)) - 0.055;
    return select(lo, hi, color > vec3f(0.0031308));
}

// ---- np-gfx-post: DC3 pseudo-HDR bloom mask source ----
// Retail DC3 standard.ps / particles.ps with option bit 22 (mPseudoHDR, set for
// NgMat::AllowHDR materials drawn into the main frame) write
//   alpha = sat(dot(rgb, c7) * c5.z),  c7 = (0.3, 0.59, 0.11) * SetBloomColor's
//   scale, c5.z = NgMat::mBloomMultiplier
// where rgb is the colour BEFORE the 8-bit target clamps it, in the units it is
// stored in (retail shades in gamma space: COLOUR_SPACE.md). So a lit value
// above 1 blooms. Natively that is the colour before the highlight clamp /
// compression, put into the units the shader writes: encoded when the shader
// encodes (linear shading), as is when it shades in gamma space. The material
// carries -(c7 scale * c5.z) in bloomMaskScale (rndshape::BloomMaskScale, DC3
// shape); RB3's positive scale keeps its own clamped-colour mask above.
fn dc3BloomMaskSource(preTone: vec3f, encode: bool) -> vec3f {
    let c = max(preTone, vec3f(0.0));
    if (encode) {
        return linearToSrgb(c);
    }
    return c;
}
// ---- end np-gfx-post ----

// === Vertex Shader (static meshes) ===
@vertex
fn vs_main(vertex: VertexInput) -> VertexOutput {
    var vertexOutput: VertexOutput;

    let worldPos = (object.world * vec4f(vertex.position, 1.0)).xyz;
    let worldNormal = normalize((object.worldInvTranspose * vec4f(vertex.normal, 0.0)).xyz);
    let viewDir = normalize(scene.cameraPos - worldPos);
    let worldTangent = normalize((object.world * vec4f(vertex.tangent.xyz, 0.0)).xyz);

    vertexOutput.clipPos = scene.viewProj * vec4f(worldPos, 1.0);
    vertexOutput.worldPos = worldPos;
    vertexOutput.worldNormal = worldNormal;
    vertexOutput.color = vertex.color;
    vertexOutput.uv = computeTexGenUV(vertex.uv, worldPos, worldNormal, viewDir);
    vertexOutput.worldTangent = worldTangent;
    vertexOutput.worldBitangent = cross(worldNormal, worldTangent) * vertex.tangent.w;
    vertexOutput.isSkinned = 0.0;
    let retailAO = retailAOFactors(worldPos, vertex.normal, vertex.color);
    vertexOutput.retailAO = retailAO;
    vertexOutput.vertexLit = retailVertexLight(worldPos, worldNormal, retailAO);
    vertexOutput.vertexSpec = retailVertexSpecular(worldPos, worldNormal, retailAO);

    return vertexOutput;
}

// === Vertex Shader (skinned meshes) ===
@vertex
fn vs_skinned(vertex: SkinnedVertexInput) -> VertexOutput {
    var vertexOutput: VertexOutput;

    var blendedPosition = vec4f(0.0);
    var blendedNormal = vec4f(0.0);
    var blendedTangent = vec4f(0.0);
    let normalizedWeights = normalizeBoneWeights(vertex.boneWeights);

    for (var influenceIndex = 0u; influenceIndex < 4u; influenceIndex++) {
        let influenceWeight = boneWeightAt(normalizedWeights, influenceIndex);
        if (influenceWeight <= 0.0) {
            continue;
        }

        let boneMatrix = bones[boneIndexAt(vertex.boneIndices, influenceIndex)];
        blendedPosition += influenceWeight * (boneMatrix * vec4f(vertex.position, 1.0));
        blendedNormal += influenceWeight * (boneMatrix * vec4f(vertex.normal, 0.0));
        blendedTangent += influenceWeight * (boneMatrix * vec4f(vertex.tangent.xyz, 0.0));
    }

    let worldPos = (object.world * vec4f(blendedPosition.xyz, 1.0)).xyz;
    let worldNormal = normalize((object.worldInvTranspose * vec4f(blendedNormal.xyz, 0.0)).xyz);
    let viewDir = normalize(scene.cameraPos - worldPos);
    let worldTangent = normalize((object.world * vec4f(blendedTangent.xyz, 0.0)).xyz);

    vertexOutput.clipPos = scene.viewProj * vec4f(worldPos, 1.0);
    vertexOutput.worldPos = worldPos;
    vertexOutput.worldNormal = worldNormal;
    vertexOutput.color = vertex.color;
    vertexOutput.uv = computeTexGenUV(vertex.uv, worldPos, worldNormal, viewDir);
    vertexOutput.worldTangent = worldTangent;
    vertexOutput.worldBitangent = cross(worldNormal, worldTangent) * vertex.tangent.w;
    vertexOutput.isSkinned = 1.0;
    let retailAO = retailAOFactors(worldPos, vertex.normal, vertex.color);
    vertexOutput.retailAO = retailAO;
    vertexOutput.vertexLit = retailVertexLight(worldPos, worldNormal, retailAO);
    vertexOutput.vertexSpec = retailVertexSpecular(worldPos, worldNormal, retailAO);

    return vertexOutput;
}

// RB3 retail lighting of a lit material (standard.vs + standard.ps), the
// texel-and-colour product `base` in, the colour before the emissive add out.
//  * without per_pixel_lit: ambient * AO + the box map and point lights from
//    the vertex, times base, plus the per-vertex specular;
//  * with per_pixel_lit (and a light present): the same terms per pixel on the
//    normal-mapped normal, the specular per pixel with the specular map, and
//    the rim light. Normal map (bit 5): N = normalize(z N + k x B + k y T),
//    x, y = 2 * texel.rg - 1, z = sat(1 - x^2 - y^2), k = 1 - deNormal (c14),
//    red along the bitangent cross(N, T) * T.w and green along the tangent.
//    Specular map (bit 1): colour * texel.rgb, power max(power * texel.a, 0.5).
//    Rim (bit 37): r = pow((1 - sat(N . V)) (0.5 N.z + 0.5), power) * colour
//    * box(-V), the box map seen from behind; the diffuse product is scaled by
//    1 + 0.3 r and 0.7 r is added. A rim map (bit 15) scales the colour by its
//    texel.rgb and the power by its texel.a, at least 0.5.
// DC3 projected light (rndshape: DC3SceneLighting.cpp WriteDC3ProjShadow;
// docs/native/dc3-retail-lighting.md section 3). At most one per environ:
// scene.projLightColor.w is 1 for projected_blend 1, 2 for projected_blend 0,
// 0 for none, and it applies to materials with recv_proj_lights
// (object.retail.z). Both forms take
//   uv = (c95 . p, c96 . p) / (c97 . p)   (scene.lightViewProj, rebuilt so that
//   the rasteriser's texel for p is this uv), s = tf5(uv).a
// and differ in what they do with it:
//   blend 1 (0x100010030011): box(N) c0 is scaled by 1 - 0.75 s c69 sat(c65 . N);
//   blend 0 (0x10030011):     (1 - s) tf10(uv).rgb c69 sat(c65 . N) joins the
//                             box term (gobo border 0 outside the cone).
// tf5 is NgLight's 256x256 coverage target after BlurShadowRT's two 5-tap
// passes (weights 0.1 0.25 0.3 0.25 0.1); a light without shadow objects
// binds the FlatNormal default, alpha 0, so s = 0 (scene.shadowEnabled < 1.5).
// Here: ShadowPass's map holds the casters at depth 0 over 1, and nine
// compare taps 1.5 texels of 256 apart with weights (1 2 1)^2 / 16 stand in
// for the blur (sigma 1.06 vs 1.14 texels).
struct Dc3Proj {
    mult: vec3f,   // scales box(N)
    add: vec3f,    // joins box(N)
}

fn dc3ProjLight(worldPos: vec3f, N: vec3f) -> Dc3Proj {
    var r: Dc3Proj;
    r.mult = vec3f(1.0);
    r.add = vec3f(0.0);
    let mode = scene.projLightColor.w;
    if (mode < 0.5 || object.retail.z < 0.5) {
        return r;
    }
    let clip = scene.lightViewProj * vec4f(worldPos, 1.0);
    let w = select(1.0, clip.w, abs(clip.w) > 1e-6);
    let uv = vec2f(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5);
    let inside = clip.w > 0.0 && all(uv >= vec2f(0.0)) && all(uv <= vec2f(1.0));
    var coverage = 0.0;
    if (scene.shadowEnabled > 1.5) {
        let t = 1.5 / 256.0;
        var lit = 0.0;
        lit += 1.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(-t, -t), 0.5);
        lit += 2.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(0.0, -t), 0.5);
        lit += 1.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(t, -t), 0.5);
        lit += 2.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(-t, 0.0), 0.5);
        lit += 4.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv, 0.5);
        lit += 2.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(t, 0.0), 0.5);
        lit += 1.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(-t, t), 0.5);
        lit += 2.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(0.0, t), 0.5);
        lit += 1.0 * textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(t, t), 0.5);
        coverage = select(0.0, 1.0 - lit / 16.0, inside);
    }
    let lightTerm = scene.projLightColor.rgb * saturate(dot(scene.projLightDir.xyz, N));
    if (mode < 1.5) {
        r.mult = vec3f(1.0) - 0.75 * coverage * lightTerm;
    } else {
        let gobo = textureSampleLevel(projLightTex, projLightSampler, uv, 0.0).rgb;
        r.add = select(vec3f(0.0), (1.0 - coverage) * gobo * lightTerm, inside);
    }
    return r;
}

// The box map sampled along a direction with a Phong exponent per face:
// pow(sat(+-R.axis), p) times the face colour.
fn boxLightPow(R: vec3f, power: f32) -> vec3f {
    var box = vec3f(0.0);
    box += pow(saturate( R.x), power) * object.boxLight[0].rgb;
    box += pow(saturate(-R.x), power) * object.boxLight[1].rgb;
    box += pow(saturate( R.y), power) * object.boxLight[2].rgb;
    box += pow(saturate(-R.y), power) * object.boxLight[3].rgb;
    box += pow(saturate( R.z), power) * object.boxLight[4].rgb;
    box += pow(saturate(-R.z), power) * object.boxLight[5].rgb;
    return box;
}

// DC3's skin and hair (RndMat shader_variation 1 and 2): the per-pixel
// standard.ps permutations 0000032040035037 (skin, rim, rim-under, normal and
// specular maps, 3 points) and 0000030080031017 (hair, specular map, 3
// points), read from the shipped microcode and checked against an emulation of
// it to 1e-7 over random inputs (docs/native/dc3-retail-lighting.md, "Skin and
// hair"). DC3 only: scene.retailLighting 2.
//
// Skin. Nk is the normal-mapped normal with the deNormal factor, Nf the same
// normal at full strength; L_i, c_i, a_i a point light's direction, colour,
// falloff; p = max(spec map a * c2.w, 0.5); n(p) = (p + 2) / 2 pi.
//   diffuse light = (1 + rim box(-V)) box(Nk) + sum a_i c_i wrap(Nk . L_i),
//     wrap(x) = sat(x (0.55, 0.6, 0.65) + (0.45, 0.4, 0.35))   (red wraps most)
//     rim = ((1 - sat(V . Nk)) (0.5 Nk.z + 0.5))^rimPower
//   colour = texel c0 (light + ambient) (1 + rimColour back)
//     back = sum a_i c_i sat(-L_i . V) (1 - sat(V . Nf)^2)^rimPower
//   specular = spec (sum a_i c_i 4 (1 - sat(V . Nf)^2)^0.7 n(p) sat(V . R_i)^p
//                    + rimf^2 n(p) boxPow(Re, p)),
//     R_i = reflect(-L_i, Nf), Re = reflect(-V, Nf),
//     rimf = (1 - sat(V . Nf)) (0.5 Nf.z + 0.5)
// Without a rim colour (option bit 37 clear) the rim and back terms are absent.
struct Dc3CharLight {
    diffuse: vec3f,    // multiplies the texel (ambient included)
    spec: vec3f,       // added
}

fn dc3SkinLight(worldPos: vec3f, Nk: vec3f, Nf: vec3f, V: vec3f, ao: vec4f,
                ambientTerm: vec3f, proj: Dc3Proj, specColor: vec3f, power: f32,
                hasSpec: bool) -> Dc3CharLight {
    var r: Dc3CharLight;
    let hasRim = dot(abs(material.retailRim.rgb), vec3f(1.0)) > 0.0;
    let rimPower = material.retailRim.a;
    let vnf = saturate(dot(V, Nf));
    let sin2 = 1.0 - vnf * vnf;
    var rimLift = 0.0;
    var sheen = 0.0;
    if (hasRim) {
        rimLift = pow(max((1.0 - saturate(dot(V, Nk))) * (0.5 * Nk.z + 0.5), 0.0), rimPower);
        sheen = pow(sin2, rimPower);
    }
    let boxN = ao.x * boxLightSample(Nk);
    var light = (1.0 + rimLift * boxLightSample(-V)) * boxN + (proj.mult - vec3f(1.0)) * boxN +
        proj.add;
    var back = vec3f(0.0);
    var spec = vec3f(0.0);
    let lobe = power * 0.1591549 + 0.3183099;
    let fres = 4.0 * pow(sin2, 0.7);
    let pointLightCount = i32(scene.numPointLights);
    for (var i = 0; i < 4; i++) {
        if (i >= pointLightCount) {
            break;
        }
        let L = retailPointDir(i, worldPos);
        let c = retailPointAO(ao, i) * retailPointColor(i, worldPos);
        light += c * saturate(dot(Nk, L) * vec3f(0.55, 0.6, 0.65) + vec3f(0.45, 0.4, 0.35));
        back += c * saturate(-dot(L, V)) * sheen;
        let R = 2.0 * dot(Nf, L) * Nf - L;
        spec += c * fres * lobe * pow(saturate(dot(V, R)), power);
    }
    let Re = 2.0 * dot(V, Nf) * Nf - V;
    let rimf = (1.0 - vnf) * (0.5 * Nf.z + 0.5);
    spec += rimf * rimf * lobe * ao.x * boxLightPow(Re, power);
    r.diffuse = (ao.x * ambientTerm + light) * (vec3f(1.0) + material.retailRim.rgb * back);
    r.spec = select(vec3f(0.0), specColor * spec, hasSpec);
    return r;
}

// Hair. N the normal (the shipped permutation has no normal map), n the
// interpolated normal before normalising, Re = reflect(-V, N), and the strand
// direction taken as horizontal: t = Re . (z x n) = n.x Re.y - n.y Re.x.
//   diffuse light = box(N) + sum a_i c_i wrap(N . L_i),
//     wrap(x) = sat(x (0.75, 0.8, 0.85) + (0.25, 0.2, 0.15))
//   s = |1 - t^2|, l1 = s^(4p), aniso = c2 l1 + c19 s^(2p) (1 - l1)
//     (c2 the specular colour, c19 the second specular colour)
//   specular = spec map rgb (sum a_i c_i sat(L_i . Re) n(p) aniso
//                            + (0.25 + (1 - sat(V . N)) (0.5 N.z + 0.5)) n(p) boxPow(Re, p))
// The box reflection is not tinted by c2; only the map tints it.
fn dc3HairLight(worldPos: vec3f, N: vec3f, nRaw: vec3f, V: vec3f, ao: vec4f,
                ambientTerm: vec3f, proj: Dc3Proj, specMap: vec3f, power: f32,
                hasSpec: bool) -> Dc3CharLight {
    var r: Dc3CharLight;
    let boxN = ao.x * boxLightSample(N);
    var light = boxN + (proj.mult - vec3f(1.0)) * boxN + proj.add;
    let Re = 2.0 * dot(V, N) * N - V;
    let t = nRaw.x * Re.y - nRaw.y * Re.x;
    let s = abs(1.0 - t * t);
    let l1 = pow(s, 4.0 * power);
    let aniso = material.retailSpec.rgb * l1 +
        material.specular2Color.rgb * pow(s, 2.0 * power) * (1.0 - l1);
    let lobe = power * 0.1591549 + 0.3183099;
    var spec = vec3f(0.0);
    let pointLightCount = i32(scene.numPointLights);
    for (var i = 0; i < 4; i++) {
        if (i >= pointLightCount) {
            break;
        }
        let L = retailPointDir(i, worldPos);
        let c = retailPointAO(ao, i) * retailPointColor(i, worldPos);
        light += c * saturate(dot(N, L) * vec3f(0.75, 0.8, 0.85) + vec3f(0.25, 0.2, 0.15));
        spec += c * saturate(dot(L, Re)) * lobe * aniso;
    }
    let rimb = (1.0 - saturate(dot(V, N))) * (0.5 * N.z + 0.5);
    spec += (0.25 + rimb) * lobe * ao.x * boxLightPow(Re, power);
    r.diffuse = ao.x * ambientTerm + light;
    r.spec = select(vec3f(0.0), specMap * spec, hasSpec);
    return r;
}

// The per-pixel half of retailShade, after the texture fetches. Everything it
// reads besides the scene, material and object uniforms is an argument, so a
// compute shader can run it on chosen inputs
// (dc3-decomp native/tests/test_retail_char_shading.cpp).
struct RetailPixel {
    worldPos: vec3f,
    normal: vec3f,        // interpolated, not normalised (hair reads it raw)
    tangent: vec3f,
    bitangent: vec3f,
    normalSample: vec4f,
    specSample: vec4f,
    rimSample: vec4f,
    ao: vec4f,            // retailAOFactors
}

fn retailPixelShade(p: RetailPixel, base: vec3f, ambientTerm: vec3f, proj: Dc3Proj) -> vec3f {
    let ao = p.ao;
    let normalSample = p.normalSample;
    let specSample = p.specSample;
    let rimSample = p.rimSample;
    var N = normalize(p.normal);
    var Nfull = N;   // the normal map at full strength (skin's specular)
    if (isEnabled(material.retailFlags.y)) {
        let x = normalSample.r * 2.0 - 1.0;
        let y = normalSample.g * 2.0 - 1.0;
        let z = saturate(1.0 - x * x - y * y);
        let k = 1.0 - material.deNormal;
        N = normalize(z * p.normal + k * x * p.bitangent + k * y * p.tangent);
        Nfull = normalize(z * p.normal + x * p.bitangent + y * p.tangent);
    }
    let worldPos = p.worldPos;
    let V = normalize(scene.cameraPos - worldPos);

    // DC3 skin and hair (dc3SkinLight / dc3HairLight above).
    let skin = shaderVariationIs(material.shaderVariation, kShaderVariationSkin);
    let hair = shaderVariationIs(material.shaderVariation, kShaderVariationHair);
    if (scene.retailLighting > 1.5 && (skin || hair)) {
        var specColor = material.retailSpec.rgb;
        var power = material.retailSpec.a;
        var specMap = vec3f(1.0);
        if (isEnabled(material.retailFlags.z)) {
            specMap = specSample.rgb;
            specColor *= specSample.rgb;
            power = max(power * specSample.a, 0.5);
        }
        var c: Dc3CharLight;
        if (skin) {
            c = dc3SkinLight(worldPos, N, Nfull, V, ao, ambientTerm, proj, specColor, power,
                             retailHasSpecular());
        } else {
            c = dc3HairLight(worldPos, N, p.normal, V, ao, ambientTerm, proj,
                             specMap, power, retailHasSpecular());
        }
        return base * c.diffuse + c.spec;
    }
    var diffuse = base * (ao.x * ambientTerm + retailDiffuseLight(worldPos, N, ao) +
                          (proj.mult - vec3f(1.0)) * ao.x * boxLightSample(N) + proj.add);

    var spec = vec3f(0.0);
    if (retailHasSpecular()) {
        var specColor = material.retailSpec.rgb;
        var power = material.retailSpec.a;
        if (isEnabled(material.retailFlags.z)) {
            specColor *= specSample.rgb;
            power = max(power * specSample.a, 0.5);
        }
        spec = specColor * retailSpecular(worldPos, N, V, ao, power);
    }

    var rimAdd = vec3f(0.0);
    if (dot(abs(material.retailRim.rgb), vec3f(1.0)) > 0.0) {
        var rimColor = material.retailRim.rgb;
        var rimPower = material.retailRim.a;
        if (isEnabled(material.retailFlags.w)) {
            rimColor *= rimSample.rgb;
            rimPower = max(rimPower * rimSample.a, 0.5);
        }
        let NdotV = saturate(dot(N, V));
        let up = 0.5 * N.z + 0.5;
        let rimBase = (1.0 - NdotV) * up;
        var rimLight = pow(max(rimBase, 0.0), rimPower) * boxLightSample(-V);
        if (scene.retailLighting > 1.5) {
            // DC3's standard.ps (0000032000031037) also lights the rim from
            // behind: each point light adds sat(-L . V) (1 - sat(N . V)^2)^p
            // (0.5 N.z + 0.5) of its colour.
            let sheen = pow(1.0 - NdotV * NdotV, rimPower) * up;
            let pointLightCount = i32(scene.numPointLights);
            for (var i = 0; i < 4; i++) {
                if (i >= pointLightCount) {
                    break;
                }
                rimLight += retailPointAO(ao, i) * retailPointColor(i, worldPos) *
                    saturate(-dot(retailPointDir(i, worldPos), V)) * sheen;
            }
        }
        let rim = rimColor * rimLight;
        diffuse *= vec3f(1.0) + 0.3 * rim;
        rimAdd = 0.7 * rim;
    }
    return diffuse + rimAdd + spec;
}

fn retailShade(surfaceInput: VertexOutput, base: vec3f, ambientLight: vec3f, rawVtx: vec3f,
               prelitAmbient: bool) -> vec3f {
    let ao = surfaceInput.retailAO;
    var ambientTerm = ambientLight;
    if (prelitAmbient) {
        ambientTerm = rawVtx * ambientLight;
    }
    let perPixel = isEnabled(material.retailFlags.x) && retailLightsOn();
    let proj = dc3ProjLight(surfaceInput.worldPos, normalize(surfaceInput.worldNormal));
    if (!perPixel) {
        // Per-vertex lighting. The shipped cache has no vertex-lit receiver
        // permutation (every projected-light mask has per_pixel_lit), so this
        // form is native-only: it scales the interpolated light (box and
        // points) and adds the gobo.
        return base * (ao.x * ambientTerm + surfaceInput.vertexLit * proj.mult + proj.add) +
            surfaceInput.vertexSpec;
    }
    let uv = surfaceInput.uv;
    var px: RetailPixel;
    px.worldPos = surfaceInput.worldPos;
    px.normal = surfaceInput.worldNormal;
    px.tangent = surfaceInput.worldTangent;
    px.bitangent = surfaceInput.worldBitangent;
    px.normalSample = textureSample(normalMapTex, diffuseSampler, uv);
    px.specSample = textureSample(specularMapTex, diffuseSampler, uv);
    px.rimSample = textureSample(rimMapTex, diffuseSampler, uv);
    px.ao = ao;
    return retailPixelShade(px, base, ambientTerm, proj);
}

// === Fragment Shader ===
@fragment
fn fs_main(surfaceInput: VertexOutput) -> @location(0) vec4f {
    let materialAlpha = material.color.a;
    // Vertex colour is the per-vertex baked lighting / AO. In the original
    // engine it is "Use vertex color and alpha for base or ambient" — i.e. it
    // ONLY contributes to the base colour when the material is prelit
    // (RndMat::mPreLit). For non-prelit STATIC venue geometry the lighting comes
    // from the scene lights below, and the baked vertex colour must NOT be
    // multiplied into the base — doing so double-applies the venue's baked
    // (cool/purple) per-vertex lighting on top of the white scene light, which
    // washed the static scene (walls, floor, storefronts) magenta/purple.
    //
    // SKINNED character meshes (band + crowd) are the exception: their baked
    // per-vertex colour is the character's skin/cloth/hair shading, and with the
    // RB3 skin/cloth textures absent from the extracted asset set it is their
    // ENTIRE visible colour. Suppressing it there (as the flat prelit gate did)
    // rendered them as flat-WHITE silhouettes. So we keep it for skinned meshes.
    //
    // BUT the band and the crowd are the SAME mesh class (RB3 draws on-stage
    // characters as `*_extras*` / `crowd_*` skinned meshes — there is no separate
    // band-member dir), so the discriminator can NOT be band-vs-crowd. And the
    // raw baked vertex colour carries strong per-vertex chroma: kept verbatim it
    // re-casts the crowd PURPLE (empirically ~20% magenta pixels on crowd-facing
    // shots — the baked Wii AO has high-R/high-B cool lobes). The static venue is
    // even more pervasively cool (mean B-G = +0.082) which is why it must stay
    // fully suppressed.
    //
    // The fix that satisfies all three constraints (band coloured, venue NOT
    // magenta, crowd NOT magenta) is: suppress vertex colour for non-prelit
    // STATIC meshes (venue) entirely, and for non-prelit SKINNED meshes keep it
    // but DESATURATE toward luminance — this preserves the per-vertex shading /
    // value variation that stops the white-silhouette look while removing the
    // chroma that pushed the crowd purple. Prelit meshes (text/UI) keep their
    // exact authored colour as before.
    let rawVtx = surfaceInput.color.rgb;
    let vtxLuma = vec3f(dot(rawVtx, vec3f(0.299, 0.587, 0.114)));
    // Desaturate kept skinned tint to kSkinnedVtxChroma of its chroma.
    let skinnedTint = mix(vtxLuma, rawVtx, kSkinnedVtxChroma);
    var vertexTint = vec3f(1.0);
    // RB3 retail light model: standard.vs fetches the vertex colour only for
    // prelit materials. One that uses the environ takes it as its ambient
    // (vertexColour * ambient, below); one that does not shows it as authored.
    let retailLight = scene.retailLighting > 0.5;
    let prelitAmbient = retailLight && material.prelit > 1.5;
    if (retailLight) {
        if (isEnabled(material.prelit) && !prelitAmbient) {
            vertexTint = rawVtx;
        }
    } else if (isEnabled(material.prelit)) {
        vertexTint = rawVtx;                          // prelit: authored colour verbatim
    } else if (isEnabled(surfaceInput.isSkinned)) {
        vertexTint = skinnedTint;                     // skinned char: desaturated shading
    }                                                 // static non-prelit venue: white (suppress)
    // RB3 shades in gamma space (material.gammaShading): retail's standard.ps
    // multiplies the texel, the material colour and the lighting sum exactly
    // as stored and authored, and writes the product to an 8-bit
    // D3DFMT_A8R8G8B8 target. Nothing is decoded on the way in (the bitmap
    // formats carry no gamma sign, and textures upload as *Unorm here) and
    // nothing is encoded on the way out (see the end of this function), so
    // the whole computation below runs on those raw values: t * c * L.
    let gammaShading = isEnabled(material.gammaShading);
    let matRgb = material.color.rgb;
    // Alpha: retail standard.vs fetches the vertex colour only for a prelit
    // material, so a lit or unlit one takes no vertex alpha (an AO mesh's
    // vertex alpha is a spherical-harmonic band, not opacity). Without
    // per_pixel_lit, the AO permutations carry the ambient AO factor into the
    // output alpha (c1.w * ao * c0.w).
    var vertexAlpha = surfaceInput.color.a;
    if (retailLight && !isEnabled(material.prelit)) {
        vertexAlpha = 1.0;
        if (!(isEnabled(material.retailFlags.x) && retailLightsOn())) {
            vertexAlpha = surfaceInput.retailAO.x;
        }
    }
    var baseColor = vec4f(matRgb * vertexTint, materialAlpha * vertexAlpha);

    // The texel alpha colour modulation reads (1 untextured: no band tint).
    var colorModTexA = 1.0;
    if (isEnabled(material.useTexture)) {
        let diffuseSample = textureSample(diffuseTex, diffuseSampler, surfaceInput.uv);
        if (isEnabled(material.useAlphaAsRGB)) {
            baseColor = vec4f(baseColor.rgb * diffuseSample.a, baseColor.a * diffuseSample.a);
        } else {
            colorModTexA = diffuseSample.a;
            baseColor = vec4f(baseColor.rgb * diffuseSample.rgb,
                              baseColor.a * colorModAlpha(diffuseSample.a));
        }
    }

    // DC3 retail environment map (scene.retailLighting 2, standard.ps option
    // bit 3; permutations 0x30018, 0x30019, 0x80000030019 disassembled in
    // docs/native/dc3-retail-lighting.md). The cube texel is added to the
    // diffuse texel BEFORE lighting, weighted by its own alpha:
    //   (tex + env.rgb * env.a * w) * colour * lighting,
    // w = 1 - sat(N . V) with env falloff (bit 43), else 1. The lookup is the
    // reflected view vector R = 2 (N . V) N - V, handed to the cube fetch as
    // (R.x, R.z, R.y) (the `cube r1, r1.yyxz, r1.zxyy` swizzle). The option is
    // set only inside CalcShaderOpts' "real or approx light" block, for a
    // material that uses the environ.
    if (scene.retailLighting > 1.5 && isEnabled(material.environMapStrength) &&
        dc3UsesEnviron() && retailLightsOn()) {
        let envN = normalize(surfaceInput.worldNormal);
        let envV = normalize(scene.cameraPos - surfaceInput.worldPos);
        let envNdotV = dot(envN, envV);
        let envR = 2.0 * envNdotV * envN - envV;
        let envSample = textureSample(environMapTex, environSampler, vec3f(envR.x, envR.z, envR.y));
        var envWeight = envSample.a;
        if (isEnabled(material.environMapFalloff)) {
            envWeight *= 1.0 - saturate(envNdotV);
        }
        if (isEnabled(material.environMapSpecMask)) {
            envWeight *= textureSample(specularMapTex, mapSampler, surfaceInput.uv).a;
        }
        baseColor = vec4f(baseColor.rgb + matRgb * vertexTint * envSample.rgb * envWeight,
                          baseColor.a);
    }

    // Retail world refraction: the texel's rgb is the diffuse rgb times the
    // refracted frame, or the frame alone without a diffuse map.
    let refract = refractStrength();
    if (refract > 0.0) {
        baseColor = vec4f(baseColor.rgb * refractScreen(surfaceInput.worldPos, surfaceInput.uv,
                                                        refract, gammaShading),
                          baseColor.a);
    }

    // Intensify is the original material toggle that doubles texture brightness.
    baseColor = vec4f(baseColor.rgb * material.intensify, baseColor.a);

    if (baseColor.a < material.alphaThreshold) {
        discard;
    }

    let surfaceNormal = sampleSurfaceNormal(surfaceInput);
    let viewDir = normalize(scene.cameraPos - surfaceInput.worldPos);
    let specularSample = textureSample(specularMapTex, mapSampler, surfaceInput.uv);
    let isSkinMaterial = shaderVariationIs(material.shaderVariation, kShaderVariationSkin);
    let isHairMaterial = shaderVariationIs(material.shaderVariation, kShaderVariationHair);
    let isWorldProjection = shaderVariationIs(material.shaderVariation, kShaderVariationWorldProjection);
    let shadowFactor = sampleShadowFactor(surfaceInput.worldPos);

    var totalLighting = emptyLightingTerms();
    let directionalLightCount = i32(scene.numLights);
    for (var directionalLightIndex = 0; directionalLightIndex < 4; directionalLightIndex++) {
        if (directionalLightIndex >= directionalLightCount) {
            break;
        }

        let lightDir = normalize(-scene.lightDirs[directionalLightIndex].xyz);
        let lightColor = scene.lightColors[directionalLightIndex].rgb;
        let lightContribution = computeDirectionalLight(
            surfaceNormal,
            viewDir,
            lightDir,
            lightColor,
            surfaceInput.worldTangent,
            specularSample,
            isSkinMaterial,
            isHairMaterial
        );
        totalLighting.diffuse += lightContribution.diffuse;
        totalLighting.specular += lightContribution.specular;
    }

    let pointLightCount = i32(scene.numPointLights);
    for (var pointLightIndex = 0; pointLightIndex < 4; pointLightIndex++) {
        if (pointLightIndex >= pointLightCount) {
            break;
        }

        let lightContribution = computePointLight(
            surfaceNormal,
            viewDir,
            surfaceInput.worldPos,
            pointLightIndex,
            specularSample
        );
        totalLighting.diffuse += lightContribution.diffuse;
        totalLighting.specular += lightContribution.specular;
    }

    // Projected light (kFakeSpot with gobo texture)
    if (scene.numProjLights > 0.5) {
        let projLightDir = normalize(-scene.projLightDir.xyz);
        let projNdotL = max(dot(surfaceNormal, projLightDir), 0.0);
        // Project world position to gobo UV
        let projU = dot(surfaceInput.worldPos, scene.projLightProjRow0.xyz) + scene.projLightProjRow0.w;
        let projV = dot(surfaceInput.worldPos, scene.projLightProjRow1.xyz) + scene.projLightProjRow1.w;
        let projUV = vec2f(projU, projV);
        // textureSample must be in uniform control flow (numProjLights is uniform),
        // so sample unconditionally here and gate the contribution with the UV check.
        let goboSample = textureSample(projLightTex, projLightSampler, projUV);
        // Only light surfaces within the projection cone (UV in [0,1])
        if (projU >= 0.0 && projU <= 1.0 && projV >= 0.0 && projV <= 1.0) {
            let projContrib = scene.projLightColor.rgb * goboSample.rgb * projNdotL;
            totalLighting.diffuse += projContrib;
        }
    }

    let ambientLight = scene.ambientColor.rgb;
    var finalColor: vec3f;
    // prelit OR unlit ⇒ register color only (no ambient/lights). For unlit, the
    // CPU side forces a white vertexTint (non-prelit static mesh), so baseColor.rgb
    // is material.color × texture — the Wii GX_SRC_REG / mUseEnviron==0 look.
    // Emissive/rim/reflection adds still apply below (night-city glow stays).
    if ((isEnabled(material.prelit) && !prelitAmbient) || isEnabled(material.unlit)) {
        finalColor = baseColor.rgb;
        if (retailLight && isEnabled(material.unlit)) {
            // use_environ=0 keeps an ambient of 1 (NgMat::SetupAmbient), and
            // the AO permutation still scales it.
            finalColor *= surfaceInput.retailAO.x;
        }
    } else if (retailLight) {
        finalColor = retailShade(surfaceInput, baseColor.rgb, ambientLight, rawVtx, prelitAmbient);
    } else {
        // GX-faithful: soft-saturate the (ambient + diffuse) lighting sum before
        // it modulates the texture so a hot multi-light venue moment tints the
        // surface without blowing it past the texture's own brightness to white.
        // Specular is an additive glint term and is left unclamped (compress-
        // Highlights handles its tail) so legitimate highlights survive.
        let litTerm = softClipLighting(ambientLight + totalLighting.diffuse * shadowFactor);
        finalColor =
            baseColor.rgb * litTerm +
            totalLighting.specular * shadowFactor;
    }

    let emissiveSample = textureSample(emissiveMapTex, mapSampler, surfaceInput.uv);
    // Emissive (self-illumination). The diffuse base color tints the emissive map
    // for materials that intend it (white-base buildings/signs stay neutral;
    // colour-base venue streaks/neons keep their hue). But a (near-)black base
    // must NOT collapse self-illumination to zero: pure-additive glow materials
    // (e.g. gem_smasher_glow.mat, color=0,0,0, blendAdd) author their colour in
    // the emissive MAP and rely on a script-driven base the native port can't
    // drive. For those, fall back to a white emissive tint so the map shows at
    // authored intensity. Only materials whose base is essentially black are
    // affected — they emit nothing today, so this introduces no regression for
    // any material that's currently visible.
    //
    // RB3 retail (gammaShading) does not tint it: each standard.ps
    // permutation with the glow bit (option bit 7) scales the emissive texel
    // by c5.x alone and adds it as its own term, e.g.
    // `mad oC0.xyz, emissive.rgb, c5.xxxx, <lit diffuse>`. c5.x is
    // NgMat::SetRegularShaderConst's mEmissiveMultiplier, so the map is
    // added as authored on top of texel * colour * lighting.
    let emBaseLuma = max(max(baseColor.r, baseColor.g), baseColor.b);
    var emissiveTint = mix(vec3f(1.0), baseColor.rgb, smoothstep(0.0, 0.04, emBaseLuma));
    if (gammaShading) {
        emissiveTint = vec3f(1.0);
    }
    finalColor += emissiveTint * material.emissiveMultiplier * emissiveSample.rgb;
    // DC3's retail model has its rim inside retailShade and its environment
    // map in the texel (above); the native rim and reflection adds are the
    // other models'.
    if (scene.retailLighting < 1.5) {
        finalColor += computeRimLighting(surfaceNormal, viewDir, surfaceInput.uv);
        finalColor += computeEnvironmentReflection(surfaceNormal, viewDir, specularSample);
    }
    // RB3 colour modulation scales the shaded colour, additive terms included:
    // DC3 permutation 0x300431010 computes texel * lighting + the interpolated
    // add, then multiplies by the band factors.
    finalColor *= colorModScale(colorModTexA);

    if (isEnabled(scene.fogEnabled) && isEnabled(material.materialFogEnabled)) {
        var fogFactor: f32;
        if (scene.fogEnabled > 1.5) {
            // DC3 retail (standard.vs option bit 18, permutation 0x70010):
            // exp_sat(-2.88539 * clip.w * c91.y), c91.y = 1 / (end - start).
            let clipW = (scene.viewProj * vec4f(surfaceInput.worldPos, 1.0)).w;
            fogFactor = saturate(exp2(-2.8853900 * clipW / (scene.fogEnd - scene.fogStart)));
        } else {
            let cameraDistance = length(surfaceInput.worldPos - scene.cameraPos);
            fogFactor = clamp((scene.fogEnd - cameraDistance) / (scene.fogEnd - scene.fogStart), 0.0, 1.0);
        }
        finalColor = mix(scene.fogColor, finalColor, fogFactor);
    }

    // RB3 retail colour adjustment (standard.ps option bit 21, constants
    // c109..c111): the environ's colour transform applied to the output
    // colour, last.
    if (retailLight && scene.colorAdjust > 0.5) {
        let c4 = vec4f(finalColor, 1.0);
        finalColor = vec3f(dot(scene.colorXfm[0], c4), dot(scene.colorXfm[1], c4),
                           dot(scene.colorXfm[2], c4));
    }

    // np-gfx-post: the colour before the target clamps or compresses it, for
    // the DC3 bloom mask (dc3BloomMaskSource below).
    let bloomMaskPreTone = finalColor;

    if (gammaShading) {
        // Retail: the 8-bit target saturates the product; no shoulder.
        finalColor = clamp(finalColor, vec3f(0.0), vec3f(1.0));
    } else if (scene.venueHighlightLumaMode > 0.5) {
        finalColor = compressHighlightsLuma(finalColor);
    } else {
        finalColor = compressHighlights(finalColor);
    }

    // Fade RGB down with alpha for nearly transparent texels to suppress fringes.
    // Black is the identity of every blend this fade is kept for (the alpha
    // blends weight by alpha anyway; Add/Subtract/Screen/Lighten add nothing).
    // Multiply and Darken are skipped per pipeline: see kSkipFringeFade.
    // A native heuristic (no retail counterpart), so it must not fire when the
    // texel alpha is not coverage -- see colorModAlphaIsBandSelector.
    if (baseColor.a < kAlphaFringeFadeStart && !kSkipFringeFade && !colorModAlphaIsBandSelector()) {
        finalColor *= baseColor.a / kAlphaFringeFadeStart;
    }

    // W9: encode linear -> sRGB for the non-sRGB web surface (see linearToSrgb).
    // Except into a render target whose consumer re-encodes on sampling
    // (scene.outputLinear): encoding twice brightens everything drawn through
    // the target. A gamma-space draw (RB3) is already in the target's units,
    // in the frame and in a render target alike, so it is never encoded.
    if (!gammaShading && scene.outputLinear < 0.5) {
        finalColor = linearToSrgb(finalColor);
    }

    // RB3 pseudo-HDR bloom mask (standard.ps ShaderOptions bit 22): alpha is the
    // luma of the colour written, scaled by SetBloomColor's c7. Only set for
    // AllowHDR materials in the main frame, whose pipelines then write alpha.
    if (material.bloomMaskScale > 0.0) {
        let mask = dot(finalColor, vec3f(0.3, 0.59, 0.11)) * material.bloomMaskScale;
        return vec4f(finalColor, clamp(mask, 0.0, 1.0));
    }
    // ---- np-gfx-post: DC3 pseudo-HDR bloom mask (negative bloomMaskScale) ----
    if (material.bloomMaskScale < 0.0) {
        let src = dc3BloomMaskSource(bloomMaskPreTone, !gammaShading && scene.outputLinear < 0.5);
        let mask = dot(src, vec3f(0.3, 0.59, 0.11)) * -material.bloomMaskScale;
        return vec4f(finalColor, clamp(mask, 0.0, 1.0));
    }
    // ---- end np-gfx-post ----

    return vec4f(finalColor, baseColor.a);
}
)WGSL"
