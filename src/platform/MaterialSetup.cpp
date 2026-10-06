// DC3 Native Port — Material Setup
// Fills MaterialUniforms, resolves texture views, and builds sampler descriptors
// for both primary materials and multi-pass materials.

#include "platform/MaterialSetup.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "gfx/FrameCapture.h"
#include "rndobj/Mat.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/CubeTex.h"
#include "math/Mtx.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

// Detect venue TV/arcade screen materials that expect a dynamically-assigned
// Kinect camera texture (video_recorder.srec). On Xbox, the ScreenRecorder fills
// Screen.tex with the Kinect feed; on native there is no Kinect. Materials whose
// name ends with "Screen.mat" and lack a diffuse texture should render as opaque
// black (TV is off) rather than the white material base color.
static bool IsScreenMaterial(const char* name) {
    if (!name) return false;
    const char* dot = strstr(name, "Screen.mat");
    // Match names like "Screen.mat", "Arcade_A_Screen.mat", "pinball_Screen.mat"
    return dot && dot[10] == '\0';
}

// Simple render mode (MILO_SIMPLE_RENDER=1): skip multiply override, force prelit,
// minimal material processing. For isolating shader/blend regressions.
static bool sSimpleRender = false;
static bool sSimpleRenderChecked = false;
static bool IsSimpleRender() {
    if (!sSimpleRenderChecked) {
        sSimpleRender = (getenv("MILO_SIMPLE_RENDER") != nullptr);
        sSimpleRenderChecked = true;
        if (sSimpleRender) printf("DC3 Native: SIMPLE RENDER MODE enabled\n");
    }
    return sSimpleRender;
}

// Helper: resolve a material texture map, triggering upload if needed
static wgpu::TextureView ResolveMap(RndTex* tex, wgpu::TextureView& fallback) {
    if (!tex) return fallback;
    tex->PresyncBitmap();
    wgpu::TextureView v = GetGpuTexView(tex);
    return v ? v : fallback;
}

// TexGen mode + the 2x3 texture transform the vertex shader applies for the
// transform-driven modes. Shared by the primary material and every NextPass
// material: a pass whose mode is kTexGenXfm but whose rows are left zero maps
// every vertex to the same texel.
template <class MatT>
static void FillTexGen(const MatT& mat, MaterialUniforms& uni) {
    uni.texGenMode = (float)mat->GetTexGen();
    if (mat->GetTexGen() == kTexGenXfm || mat->GetTexGen() == kTexGenXfmOrigin ||
        mat->GetTexGen() == kTexGenProjected) {
        const Transform& xfm = mat->TexXfm();
        uni.texXfmRow0[0] = xfm.m.x.x; uni.texXfmRow0[1] = xfm.m.x.y;
        uni.texXfmRow0[2] = xfm.v.x;   uni.texXfmRow0[3] = xfm.v.z;
        uni.texXfmRow1[0] = xfm.m.y.x; uni.texXfmRow1[1] = xfm.m.y.y;
        uni.texXfmRow1[2] = xfm.v.y;   uni.texXfmRow1[3] = 0.0f;
    }
}

// RB3's retail material terms (rndshape::MatRetailTerms, doc section 12):
// the uniforms the shader's retail branch reads, and the normal, specular and
// rim maps bound in the slots the dc3 model would use. The dc3 model's own
// fields (hasNormalMap, specularPower, rimColor, ...) stay zero for these
// materials, so its terms never run on top of the retail ones.
template <class MatT>
static void FillRetailTerms(const MatT& mat, MaterialUniforms& uni,
                            WgpuRnd::MaterialTexViews& views) {
    rndshape::RetailMatTerms t;
    if (!rndshape::MatRetailTerms(mat, t)) return;
    if (t.specular) {
        uni.retailSpec[0] = t.specular_rgb[0];
        uni.retailSpec[1] = t.specular_rgb[1];
        uni.retailSpec[2] = t.specular_rgb[2];
    }
    uni.retailSpec[3] = t.specularPower;
    if (t.rim) {
        uni.retailRim[0] = t.rim_rgb[0];
        uni.retailRim[1] = t.rim_rgb[1];
        uni.retailRim[2] = t.rim_rgb[2];
    }
    uni.retailRim[3] = t.rimPower;
    uni.retailFlags[0] = t.perPixel ? 1.0f : 0.0f;
    uni.retailFlags[1] = t.normalMap ? 1.0f : 0.0f;
    uni.retailFlags[2] = t.specularMap ? 1.0f : 0.0f;
    uni.retailFlags[3] = t.rimMap ? 1.0f : 0.0f;
    if (t.perPixel) uni.deNormal = t.deNormal;
    if (t.normalMap) views.normal = ResolveMap(t.normalTex, gWgpuRnd->FlatNormalTexView());
    if (t.specularMap) views.specular = ResolveMap(t.specularTex, gWgpuRnd->WhiteTexView());
    if (t.rimMap) views.rim = ResolveMap(t.rimTex, gWgpuRnd->WhiteTexView());
}

MaterialParams BuildMaterialParams(RndMat* rawMat, bool isTextMesh) {
    // DC3 BaseMaterial getters, whatever the rndobj shape (platform/rndshape/).
    auto mat = rndshape::Mat(rawMat);
    MaterialParams result{};
    MaterialUniforms& matUni = result.uniforms;
    uint32_t heuristics = 0;

    // --- Color + alpha ---
    const Hmx::Color& matColor = mat->GetColor();
    matUni.color[0] = matColor.red;
    matUni.color[1] = matColor.green;
    matUni.color[2] = matColor.blue;
    matUni.color[3] = matColor.alpha;

    BaseMaterial::Blend matBlend = mat->GetBlend();

    if (mat->GetAlphaCut()) {
        matUni.alphaThreshold = mat->GetAlphaThreshold() / 255.0f;
    } else {
        matUni.alphaThreshold = 0.0f;
    }

    // --- Specular ---
    const Hmx::Color& spec = mat->GetSpecularRGB();
    float specPower = spec.alpha > 0.0f ? spec.alpha : 0.0f;
    matUni.specularColor[0] = spec.red;
    matUni.specularColor[1] = spec.green;
    matUni.specularColor[2] = spec.blue;
    matUni.specularColor[3] = 1.0f;
    matUni.specularPower = specPower;
    matUni.hasSpecularMap = mat->GetSpecularMap() ? 1.0f : 0.0f;

    // --- Emissive ---
    // Only applies when an emissive map texture exists.
    // Without a map, emissiveMultiplier defaults to 1.0 which would add
    // the full diffuse color as self-illumination (completely wrong)
    matUni.emissiveMultiplier = mat->GetEmissiveMap() ? mat->GetEmissiveMultiplier() : 0.0f;
    if (!mat->GetEmissiveMap()) heuristics |= kHeuristicEmissiveGuard;

    // --- Rim lighting ---
    const Hmx::Color& rim = mat->GetRimRGB();
    matUni.rimColor[0] = rim.red;
    matUni.rimColor[1] = rim.green;
    matUni.rimColor[2] = rim.blue;
    matUni.rimColor[3] = rim.alpha > 0.0f ? rim.alpha : 0.0f;
    matUni.rimLightUnder = mat->GetRimLightUnder() ? 1.0f : 0.0f;

    // --- Intensify ---
    matUni.intensify = mat->GetIntensify() ? 2.0f : 1.0f;

    // --- Shader variation (skin, hair, etc.) ---
    // Verified: DC3 materials have correct shader_variation in binary data.
    // No name-based heuristic needed.
    matUni.shaderVariation = (float)mat->GetShaderVariation();

    // --- Second specular lobe (used by skin shader) ---
    const Hmx::Color& spec2 = mat->GetSpecular2RGB();
    matUni.specular2Color[0] = spec2.red;
    matUni.specular2Color[1] = spec2.green;
    matUni.specular2Color[2] = spec2.blue;
    matUni.specular2Color[3] = spec2.alpha > 0.0f ? spec2.alpha : 0.0f;

    // --- Diffuse texture ---
    RndTex* diffTex = mat->GetDiffuseTex();
    wgpu::TextureView diffuseTexView;
    if (diffTex) {
        diffTex->PresyncBitmap();
        diffuseTexView = GetGpuTexView(diffTex);
    }

    if (diffuseTexView) {
        matUni.useTexture = 1.0f;
    } else if (diffTex) {
        // Texture object exists but GPU upload failed (e.g., unsupported format,
        // render target without content). Show opaque black rather than letting the
        // material's potentially-white base color shine through.
        matUni.useTexture = 1.0f;
        diffuseTexView = gWgpuRnd->BlackTexView();
    } else if (IsScreenMaterial(mat->Name())) {
        // Venue TV/arcade screen materials whose diffuse texture is normally assigned
        // at runtime by the Kinect ScreenRecorder (video_recorder.srec). On native
        // there is no Kinect, so show opaque black (TV is off) instead of the
        // material's white base color.
        matUni.useTexture = 1.0f;
        diffuseTexView = gWgpuRnd->BlackTexView();
    } else {
        matUni.useTexture = 0.0f;
        diffuseTexView = gWgpuRnd->WhiteTexView();
    }

    // --- Normal map and additional material properties ---
    matUni.deNormal = mat->GetDeNormal();
    matUni.hasNormalMap = mat->NormalMap() ? 1.0f : 0.0f;
    matUni.anisotropy = mat->GetAnisotropy();

    // --- Per-material fog ---
    BaseMaterial::Blend blend = mat->GetBlend();
    bool allowFog = mat->GetFog() &&
        blend != BaseMaterial::kBlendDest && blend != BaseMaterial::kBlendAdd &&
        blend != BaseMaterial::kBlendSubtract && blend != BaseMaterial::kBlendSrcAlphaAdd;
    matUni.materialFogEnabled = allowFog ? 1.0f : 0.0f;
    if (!allowFog && mat->GetFog()) heuristics |= kHeuristicFogBlendCheck;

    // Force prelit for HUD overlay meshes: the overlay pass has no guaranteed
    // environment/lighting, so skip 3D lighting calculations entirely.
    // On Xbox, HUD meshes render inline with the 3D scene using their own env,
    // but on native the overlay pass is a separate 1x (no MSAA) pass.
    bool isOverlayPass = gWgpuRnd && !gWgpuRnd->CurrentPassHasDepth();
    // Force prelit for multiply-blend materials (e.g., light-catcher overlays).
    // On Xbox, DTA scripts and the full lighting pipeline set the material color
    // to represent the desired light modulation factor. On native, without those
    // scripts, the material color stays white and the lighting system adds bright
    // illumination — causing multiply blend to brighten instead of modulate.
    // Forcing prelit makes the shader output baseColor directly: white material
    // color = multiply identity (no visible change), which is correct behavior
    // when the lighting scripts aren't driving the color.
    // An unlit material (rndshape::MatUnlit; RB3-Wii use_environ=0) already
    // skips lighting, and forcing prelit on top of it would pull in the raw
    // vertex colour, which an unlit Wii material never sees.
    const bool unlit = rndshape::MatUnlit(mat);
    matUni.unlit = unlit ? 1.0f : 0.0f;
    matUni.gammaShading = rndshape::kGammaSpaceShading ? 1.0f : 0.0f;
    bool isMultiplyBlend = (matBlend == BaseMaterial::kBlendMultiply) && !unlit;
    bool forcePrelit = IsSimpleRender() || isOverlayPass || isMultiplyBlend;
    if (isMultiplyBlend) heuristics |= kHeuristicMultiplyPrelit;
    if (isTextMesh) heuristics |= kHeuristicTextMeshDetect;
    matUni.prelit = (mat->Prelit() || isTextMesh || forcePrelit) ? 1.0f : 0.0f;
    // 2 = prelit whose vertex colour is its ambient (rndshape::MatPrelitAmbient);
    // read only under the RB3 retail light model, any other path sees prelit.
    if (mat->Prelit() && !isTextMesh && !forcePrelit && rndshape::MatPrelitAmbient(mat))
        matUni.prelit = 2.0f;
    matUni.useAlphaAsRGB = isTextMesh ? 1.0f : 0.0f;
    if (isTextMesh) {
        heuristics |= kHeuristicTextAlphaAsRGB;
    }

    // --- Detail normal map ---
    matUni.normDetailTiling = mat->GetNormDetailTiling();
    matUni.normDetailStrength = mat->GetNormDetailStrength();
    matUni.hasNormDetailMap = mat->GetNormDetailMap() ? 1.0f : 0.0f;

    // --- TexGen mode and transform ---
    FillTexGen(mat, matUni);

    // --- Resolve all material texture views ---
    WgpuRnd::MaterialTexViews& texViews = result.texViews;
    texViews.diffuse = diffuseTexView;

    texViews.normal   = ResolveMap(mat->NormalMap(),      gWgpuRnd->FlatNormalTexView());
    texViews.specular = ResolveMap(mat->GetSpecularMap(), gWgpuRnd->WhiteTexView());

    texViews.emissive = ResolveMap(mat->GetEmissiveMap(), gWgpuRnd->BlackTexView());
    texViews.rim      = ResolveMap(mat->GetRimMap(),      gWgpuRnd->WhiteTexView());

    // Detail normal map
    texViews.normDetail = ResolveMap(mat->GetNormDetailMap(), gWgpuRnd->FlatNormalTexView());

    FillRetailTerms(mat, matUni, texViews);

    // --- Environment cube map ---
    RndCubeTex* environMap = mat->GetEnvironMap();
    if (environMap) {
        wgpu::TextureView cubeView = GetGpuCubeTexView(environMap);
        texViews.environCube = cubeView ? cubeView : gWgpuRnd->BlackCubeTexView();
        matUni.environMapStrength = 1.0f;
        matUni.environMapFalloff = mat->GetEnvironMapFalloff() ? 1.0f : 0.0f;
        matUni.environMapSpecMask = mat->GetEnvironMapSpecMask() ? 1.0f : 0.0f;
    } else {
        texViews.environCube = gWgpuRnd->BlackCubeTexView();
        matUni.environMapStrength = 0.0f;
    }

    // --- Sampler descriptors ---
    SamplerDesc& sampDesc = result.samplerDesc;
    switch (mat->GetTexWrap()) {
    case kTexWrapClamp:
        sampDesc.addressU = wgpu::AddressMode::ClampToEdge;
        sampDesc.addressV = wgpu::AddressMode::ClampToEdge;
        break;
    case kTexWrapRepeat:
        sampDesc.addressU = wgpu::AddressMode::Repeat;
        sampDesc.addressV = wgpu::AddressMode::Repeat;
        break;
    case kTexWrapMirror:
        sampDesc.addressU = wgpu::AddressMode::MirrorRepeat;
        sampDesc.addressV = wgpu::AddressMode::MirrorRepeat;
        break;
    default:
        sampDesc.addressU = wgpu::AddressMode::ClampToEdge;
        sampDesc.addressV = wgpu::AddressMode::ClampToEdge;
        break;
    }

    // Map sampler -- always repeat for tiled texture maps
    result.mapSamplerDesc.addressU = wgpu::AddressMode::Repeat;
    result.mapSamplerDesc.addressV = wgpu::AddressMode::Repeat;

    result.heuristics = heuristics;
    return result;
}

MaterialParams BuildPassMaterialParams(BaseMaterial* rawNextPass) {
    auto nextPass = rndshape::Mat(rawNextPass);
    MaterialParams result{};
    MaterialUniforms& npMatUni = result.uniforms;

    // --- Color ---
    const Hmx::Color& npc = nextPass->GetColor();
    npMatUni.color[0] = npc.red; npMatUni.color[1] = npc.green;
    npMatUni.color[2] = npc.blue; npMatUni.color[3] = npc.alpha;
    npMatUni.alphaThreshold = nextPass->GetAlphaCut() ? nextPass->GetAlphaThreshold() / 255.0f : 0.0f;

    // --- Specular ---
    const Hmx::Color& nps = nextPass->GetSpecularRGB();
    float npSpecPower = nps.alpha > 0.0f ? nps.alpha : 0.0f;
    npMatUni.specularPower = npSpecPower;
    npMatUni.specularColor[0] = nps.red; npMatUni.specularColor[1] = nps.green;
    npMatUni.specularColor[2] = nps.blue; npMatUni.specularColor[3] = 1.0f;
    npMatUni.hasSpecularMap = nextPass->GetSpecularMap() ? 1.0f : 0.0f;

    // --- Other properties ---
    npMatUni.emissiveMultiplier = nextPass->GetEmissiveMap() ? nextPass->GetEmissiveMultiplier() : 0.0f;
    npMatUni.intensify = nextPass->GetIntensify() ? 2.0f : 1.0f;
    npMatUni.deNormal = nextPass->GetDeNormal();
    npMatUni.hasNormalMap = nextPass->NormalMap() ? 1.0f : 0.0f;
    // Force prelit for multiply-blend passes (same rationale as primary material)
    const bool npUnlit = rndshape::MatUnlit(nextPass);
    npMatUni.unlit = npUnlit ? 1.0f : 0.0f;
    npMatUni.gammaShading = rndshape::kGammaSpaceShading ? 1.0f : 0.0f;
    bool npMultiply = (nextPass->GetBlend() == BaseMaterial::kBlendMultiply) && !npUnlit;
    npMatUni.prelit = (nextPass->Prelit() || npMultiply) ? 1.0f : 0.0f;
    if (nextPass->Prelit() && !npMultiply && rndshape::MatPrelitAmbient(nextPass))
        npMatUni.prelit = 2.0f;
    FillTexGen(nextPass, npMatUni);
    npMatUni.shaderVariation = (float)nextPass->GetShaderVariation();

    // --- Resolve textures ---
    WgpuRnd::MaterialTexViews& npTexViews = result.texViews;

    // Diffuse: a NextPass usually carries its OWN texture (e.g. RB3's
    // billboard_texture.mat multiplies a shared 256x256 grime map over every
    // adboard poster), which no primary pass ever syncs. Without this the view
    // lookup fails and the "upload failed" branch below paints the pass black.
    // PresyncBitmap is idempotent for already-uploaded textures.
    RndTex* npDiffTex = nextPass->GetDiffuseTex();
    if (npDiffTex) npDiffTex->PresyncBitmap();
    wgpu::TextureView npDiffuse = npDiffTex ? GetGpuTexView(npDiffTex) : wgpu::TextureView{};
    if (npDiffuse) {
        npMatUni.useTexture = 1.0f;
        npTexViews.diffuse = npDiffuse;
    } else if (npDiffTex) {
        // Texture exists but upload failed — show black
        npMatUni.useTexture = 1.0f;
        npTexViews.diffuse = gWgpuRnd->BlackTexView();
    } else {
        npMatUni.useTexture = 0.0f;
        npTexViews.diffuse = gWgpuRnd->WhiteTexView();
    }

    npTexViews.normal     = ResolveMap(nextPass->NormalMap(),      gWgpuRnd->FlatNormalTexView());
    npTexViews.specular   = ResolveMap(nextPass->GetSpecularMap(), gWgpuRnd->WhiteTexView());
    npTexViews.emissive   = ResolveMap(nextPass->GetEmissiveMap(), gWgpuRnd->BlackTexView());
    npTexViews.rim        = ResolveMap(nextPass->GetRimMap(),      gWgpuRnd->WhiteTexView());
    npTexViews.environCube = gWgpuRnd->BlackCubeTexView();
    npTexViews.normDetail = ResolveMap(nextPass->GetNormDetailMap(), gWgpuRnd->FlatNormalTexView());
    FillRetailTerms(nextPass, npMatUni, npTexViews);

    // Multi-pass materials don't set their own sampler -- caller reuses primary material's sampler
    result.heuristics = 0;
    return result;
}
