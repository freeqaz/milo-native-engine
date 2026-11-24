// DC3 Native Port — WebGPU Mesh Drawing
// Provides RndMesh::DrawShowing() implementation for native build.
// Uses side tables for GPU vertex/index buffers.
// Supports both static and bone-skinned meshes.

#include "platform/MeshGpuCache.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/BoneSetup.h"
#include "platform/MaterialSetup.h"
#include "platform/MeshDrawShowing.h"
#include "platform/MeshFilter.h"
#include "platform/MeshPipelineKey.h"
#include "platform/LookProfile.h"
#include "platform/TransformUtils.h"
#include "platform/TexGpu.h"
#include "gfx/FrameCapture.h"
#include "gfx/VertexFormats.h"
#include "rndobj/Mesh.h"
#include "rndobj/Mat.h"
#include "platform/rndshape/RndShape.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/Rnd.h"
#include "math/Mtx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

// Forward declaration — draws a mesh immediately (called for both opaque and deferred).
// Non-static: TransparentQueue.cpp calls this via extern linkage.
void DrawMeshImmediate(RndMesh* mesh);

// Record a draw call for frame capture diagnostics.
// Encapsulates NDC projection, texture binding info, and material uniform snapshot.
static void RecordDrawCall(
    RndMesh* mesh, RndMat* rawMat,
    const MaterialParams& matParams,
    bool skinned, int blend,
    uint32_t heuristics)
{
    auto mat = rndshape::Mat(rawMat);
    auto& rec = FrameCapture::Get().AddDraw();
    rec.meshName = MeshLabel(mesh);
    rec.materialName = mat->Name();
    RndCam* cam = RndCam::Current();
    rec.cameraName = cam ? cam->Name() : nullptr;
    RndEnviron* env = rndshape::CurrentEnv();
    rec.environName = env ? env->Name() : nullptr;
    rec.blend = blend;
    rec.zMode = mat->GetZMode();
    rec.cull = mat->GetCull();
    rec.stencil = mat->GetStencil();
    rec.skinned = skinned;
    rec.alphaCut = mat->GetAlphaCut();
    rec.alphaWrite = mat->GetAlphaWrite();
    memcpy(rec.color, matParams.uniforms.color, sizeof(rec.color));
    rec.specularPower = matParams.uniforms.specularPower;
    rec.emissiveMultiplier = matParams.uniforms.emissiveMultiplier;
    rec.prelit = matParams.uniforms.prelit;
    rec.useTexture = matParams.uniforms.useTexture;
    rec.alpha = matParams.uniforms.color[3];
    rec.texGenMode = matParams.uniforms.texGenMode;
    memcpy(rec.texXfmRow0, matParams.uniforms.texXfmRow0, sizeof(rec.texXfmRow0));
    memcpy(rec.texXfmRow1, matParams.uniforms.texXfmRow1, sizeof(rec.texXfmRow1));
    rec.texXfmTranslation[0] = mat->TexXfm().v.x;
    rec.texXfmTranslation[1] = mat->TexXfm().v.y;
    const Vector3& worldPos = mesh->WorldXfm().v;
    rec.worldPos[0] = worldPos.x;
    rec.worldPos[1] = worldPos.y;
    rec.worldPos[2] = worldPos.z;
    rec.hasNdcPos = false;
    if (cam) {
        cam->UpdatedWorldXfm();
        Transform viewXfm;
        Hmx::Matrix4 projMtx;
        rndshape::CamViewProjectXfms(cam, viewXfm, projMtx);

        float view[16] = {
            viewXfm.m.x.x, viewXfm.m.x.y, viewXfm.m.x.z, 0.0f,
            viewXfm.m.y.x, viewXfm.m.y.y, viewXfm.m.y.z, 0.0f,
            viewXfm.m.z.x, viewXfm.m.z.y, viewXfm.m.z.z, 0.0f,
            viewXfm.v.x,   viewXfm.v.y,   viewXfm.v.z,   1.0f
        };
        float proj[16] = {
            projMtx.x.x, projMtx.x.y, projMtx.x.z, projMtx.x.w,
            projMtx.y.x, projMtx.y.y, projMtx.y.z, projMtx.y.w,
            projMtx.z.x, projMtx.z.y, projMtx.z.z, projMtx.z.w,
            projMtx.w.x, projMtx.w.y, projMtx.w.z, projMtx.w.w,
        };
        float viewProj[16];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float sum = 0.0f;
                for (int k = 0; k < 4; k++) {
                    sum += view[i * 4 + k] * proj[k * 4 + j];
                }
                viewProj[i * 4 + j] = sum;
            }
        }

        float clip[4];
        float pos[4] = {worldPos.x, worldPos.y, worldPos.z, 1.0f};
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += pos[k] * viewProj[k * 4 + j];
            }
            clip[j] = sum;
        }
        if (clip[3] != 0.0f) {
            rec.ndcPos[0] = clip[0] / clip[3];
            rec.ndcPos[1] = clip[1] / clip[3];
            rec.ndcPos[2] = clip[2] / clip[3];
            rec.hasNdcPos = true;
        }
    }
    rec.heuristicsApplied = heuristics;
    // Texture binding info
    const char* slotNames[] = {"diffuse","normal","specular","emissive","rim","environCube","normDetail"};
    RndTex* slotSources[] = {
        mat->GetDiffuseTex(), mat->NormalMap(), mat->GetSpecularMap(),
        mat->GetEmissiveMap(), mat->GetRimMap(), nullptr, mat->GetNormDetailMap()
    };
    const wgpu::TextureView* slotViews[] = {
        &matParams.texViews.diffuse, &matParams.texViews.normal, &matParams.texViews.specular,
        &matParams.texViews.emissive, &matParams.texViews.rim, &matParams.texViews.environCube, &matParams.texViews.normDetail
    };
    for (int t = 0; t < 7; t++) {
        rec.texBindings[t].slotName = slotNames[t];
        rec.texBindings[t].source = slotSources[t];
        rec.texBindings[t].uploaded = (*slotViews[t]) != nullptr;
        rec.texBindings[t].usingFallback = slotSources[t] && !GetGpuTexView(slotSources[t]);
    }
}

const char* RndMeshDrawShowingSkip(RndMesh* mesh) {
    // No Showing() test, deliberately. The image's override is
    // DxMesh::DrawShowing (826229B0; 100% matched in dc3-decomp's
    // src/system/rnddx9/Mesh.cpp), and its only refusal is `!geom->CanDraw()`.
    // Showing() is gated one level up, in RndDrawable::Draw(). Everything that
    // calls DrawShowing() DIRECTLY draws the mesh whatever its flag says --
    // UIListMeshElement::Draw on a list's hidden template mesh (the case that
    // made dc3 carry a SetShowing(true)/restore workaround), RndText, RndLine,
    // RndRibbon, RndMultiMeshProxy, CharFeedback. A native-only test here
    // dropped every hidden *named* mesh on those paths.
    //
    // Tools that walk an ObjectDir and call DrawShowing() on every mesh (the
    // viewers, render-test, rb3-xenon's main_render) are bypassing Draw(), so
    // they must apply the showing gate themselves -- and do.

    // Content filters are CONSUMER policy, not engine semantics, and the engine
    // already owns a seam for them: ShouldSkipMesh (platform/MeshFilter.h), which
    // every consumer defines for itself. Two game-specific name tests used to be
    // hardcoded here instead -- a `strstr(Name(), "_lod")` LOD skip and a
    // `grid_80by60` Kinect skip. Both are DC3 assumptions, and the LOD one is
    // wrong for RB3: its crowd characters are authored *as* their LOD-2 asset
    // (char/crowd/gen/crowd_female01 ships one body mesh,
    // female_crowd_body01_lod02.mesh), so the blanket test deleted the whole
    // character. They now live in DC3's MeshFilter.cpp, where the consumer that
    // wants them can keep them and the consumers that do not are not taxed.
    //
    // DrawMeshImmediate calls ShouldSkipMesh again; it is a pure name/material
    // predicate, so the second call is free. Testing here as well keeps the skip
    // ahead of IncrementMeshDrawCalls, so the draw-call counter is unchanged.
    if (ShouldSkipMesh(mesh->Name(), mesh->Mat())) return "filtered by consumer";

    // No material test either. DxMesh::DrawShowing hands a null Mat() to
    // RndShader::SelectConfig, and RndShaderStandard::Select replaces it with
    // TheRnd.DefaultMat() (rb3-xenon 0x82738E38 and 0x824A8080, the latter 100%
    // matched; dc3-decomp's DxMesh::DrawShowing and Select are the same), so the
    // image draws a material-less mesh, colour and depth, with the default
    // material. DrawMeshImmediate does that substitution.
    return nullptr;
}

void RndMesh::DrawShowing() {
    if (!gWgpuRnd || !gWgpuRnd->IsInPass()) return;

    if (const char* skip = RndMeshDrawShowingSkip(this)) {
        if (FrameCapture::Get().IsCapturing()) FrameCapture::Get().AddSkip(Name(), skip);
        return;
    }

    // The image's DxMesh::DrawShowing refills the geometry owner's buffers
    // before drawing when the property system dirtied its verts (see
    // rndshape::MeshSyncDirtyVerts); RndMesh::Sync -> OnSync marks the GPU
    // copy for re-upload.
    if (RndMesh* geom = rndshape::MeshGeomOwner(this))
        rndshape::MeshSyncDirtyVerts(geom);

    IncrementMeshDrawCalls();
    DrawMeshImmediate(this);
}

// Draw-log identity of a uniform binding: which buffer, at which offset. Never
// dereferenced; equal tokens mean the draws bound the same uniform data.
static uint64_t DrawLogToken(const void* buffer, uint32_t offset) {
    uint64_t h = (uint64_t)(uintptr_t)buffer;
    h ^= (uint64_t)offset + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

MeshPassContext CurrentMeshPassContext() {
    MeshPassContext c;
    c.targetFormat = gWgpuRnd->CurrentTargetFormat();
    c.sampleCount = gWgpuRnd->CurrentSampleCount();
    c.hasDepth = gWgpuRnd->CurrentPassHasDepth();
    c.overlay = gWgpuRnd->CurrentPassIsOverlay();
    c.toTexture = gWgpuRnd->ActiveTargetTex() != nullptr;
    c.reflection = rndshape::TheRndRef().DrawMode() == rndshape::kDrawModeReflection;
    c.selfShadowCaster = rndshape::kSelfShadowMap
        && rndshape::TheRndRef().DrawMode() == rndshape::kSelfShadowCasterDrawMode;
    return c;
}

PipelineKey MeshPipelineKey(RndMesh* mesh, RndMat* rawMat, bool skinned, int32_t depthBias,
                            const MeshPassContext& ctx) {
    // Text meshes (created by RndText::FontMap) have empty names (not registered in ObjectDir).
    // Debug labels are stored in GpuMeshData::debugLabel for GPU debugging.
    // Only a glyph page gives up its material's depth test and culling
    // (MeshDrawsAsGlyph: a page the game marked, or -- for a game that does not
    // mark them -- an unnamed unskinned mesh). Other code builds unnamed meshes
    // too: spotlight beams, RndRibbon/RndLine, crowd billboards, and DC3's
    // HamRibbon hand trail (skinned, hamribbon.mat asks zMode 2 / cull 1).
    // Drawn as text they went through whatever stood in front of them.
    const bool isGlyphMesh = MeshDrawsAsGlyph(mesh, skinned);
    // The text colour path (forced prelit, atlas alpha as RGB) follows the same
    // mark when the game sets one (MeshShadesAsText); otherwise every unnamed
    // mesh, as before. A ribbon shaded as text happened to look right only
    // because its vertices are white.
    const bool isTextMesh = MeshShadesAsText(mesh);
    return MaterialPipelineKey(rawMat, skinned, isGlyphMesh, isTextMesh, depthBias, ctx);
}

// The cull a material draws with inside a WorldReflection (the mirrored camera
// reverses the winding). The DC3 shape forces the reverse of a regular
// material's cull on every material, as DC3's CheckForceCull does; the other
// shapes flip the material's own cull and leave a two-sided one two-sided.
WgpuCull ReflectionCull(WgpuCull materialCull, bool inReflection) {
    if (!inReflection) return materialCull;
    const int inspect = milo::ReflectionCullInspection();
    if (inspect < 0 ? rndshape::kReflectionForcesReverseCull : inspect == 1) return WgpuCull::Backwards;
    if (materialCull == WgpuCull::None) return materialCull;
    return materialCull == WgpuCull::Regular ? WgpuCull::Backwards : WgpuCull::Regular;
}

// Whether a glyph page takes its font material's z mode
// (rndshape::kGlyphKeepsMatZMode). MILO_GLYPH_ZMODE=keep|off overrides the
// shape for an A/B; MILO_GLYPH_ZMODE_LOG=1 prints each font material's z mode
// once, with whether its pass has depth.
static bool GlyphKeepsMatZMode() {
    static const int sOverride = [] {
        const char* e = getenv("MILO_GLYPH_ZMODE");
        if (!e) return -1;
        if (!strcmp(e, "keep")) return 1;
        if (!strcmp(e, "off")) return 0;
        return -1;
    }();
    return sOverride < 0 ? rndshape::kGlyphKeepsMatZMode : sOverride == 1;
}

static void LogGlyphZMode(RndMat* rawMat, int zMode, bool hasDepth) {
    static const bool sLog = getenv("MILO_GLYPH_ZMODE_LOG") != nullptr;
    if (!sLog || !rawMat) return;
    static std::set<std::pair<const void*, int>> sSeen;
    if (!sSeen.insert({rawMat, (int)hasDepth * 8 + zMode}).second) return;
    fprintf(stderr, "[glyph-zmode] mat '%s' zmode=%d pass_depth=%d keep=%d\n", rawMat->Name(),
            zMode, (int)hasDepth, (int)GlyphKeepsMatZMode());
}

PipelineKey MaterialPipelineKey(RndMat* rawMat, bool skinned, bool isGlyphMesh, bool isTextMesh,
                                int32_t depthBias, const MeshPassContext& ctx) {
    auto mat = rndshape::Mat(rawMat);
    PipelineKey key{};
    key.shaderType = 18; // kStandardShader

    BaseMaterial::Blend matBlend = mat->GetBlend();

    // Overlay pass (no depth buffer) = HUD/2D elements. Disable face culling
    // because Xbox D3D9 uses CW front face while WebGPU uses CCW. HUD quads
    // authored for CW winding are back-facing in CCW and would be culled.
    const bool isOverlayPass = !ctx.hasDepth;

    key.blend = (WgpuBlend)matBlend;
    if (isGlyphMesh) LogGlyphZMode(rawMat, (int)mat->GetZMode(), ctx.hasDepth);
    key.zMode = isGlyphMesh && !GlyphKeepsMatZMode() ? (WgpuZMode)0 : (WgpuZMode)mat->GetZMode();
    WgpuCull matCull = (isGlyphMesh || isOverlayPass) ? WgpuCull::None : (WgpuCull)mat->GetCull();
    // Reflection mode (rndshape::kDrawModeReflection) flips the camera, reversing winding order.
    // Flip cull mode so front faces aren't discarded.
    matCull = ReflectionCull(matCull, ctx.reflection && !isGlyphMesh && !isOverlayPass);
    if (ctx.selfShadowCaster && !isGlyphMesh && !isOverlayPass) matCull = WgpuCull::Backwards;
    key.cull = matCull;
    key.stencil = (WgpuStencil)mat->GetStencil();
    key.layout = skinned ? VertexLayoutType::Skinned : VertexLayoutType::Static;
    key.targetFormat = ctx.targetFormat;
    key.sampleCount = ctx.sampleCount;
    key.hasDepth = ctx.hasDepth;
    key.alphaCut = mat->GetAlphaCut();
    key.alphaWrite = mat->GetAlphaWrite();
    if (rndshape::kRenderTargetForcesAlphaWrite && ctx.toTexture
        && !rndshape::MatPinsAlphaWrite(rawMat))
        key.alphaWrite = true;
    // RB3's pseudo-HDR bloom mask (rndshape::BloomMaskScale; 0 for DC3): an
    // AllowHDR material drawn into the main frame writes its luma to alpha.
    // Not in the overlay pass, which comes after the post chain read the mask
    // (MeshPassContext::overlay; it has depth under rndshape::kOverlayDepth).
    const float bloomMaskScale = (!isOverlayPass && !ctx.overlay && !ctx.toTexture)
        ? rndshape::BloomMaskScale(mat) : 0.0f;
    if (bloomMaskScale != 0.0f)  // DC3: negative (rndshape::BloomMaskScale)
        key.alphaWrite = true;
    const CutCoverage cutCov = MaterialCutCoverage(rawMat);
    key.alphaToCoverage = cutCov == CutCoverage::AlphaToCoverage;
    key.cutoutCoverage = cutCov == CutCoverage::Sharpened;
    key.depthBias = depthBias;
    // The look's texture LOD bias (LookSettings::lodBiasScale1/2) is for
    // world textures; text and HUD never take it. Their samplers keep the
    // original's two mip levels (MaterialSetup ApplyLookSampling), and under
    // enhanced glyph pages take a bias of their own inside that clamp
    // (np-look34, measured by item 34b: see MaterialSetup.h).
    key.lodBias = MaterialDrawTakesLodBias(isTextMesh || isOverlayPass,
                                           gWgpuRnd->Pipelines().MaterialTakesLodBias());
    // ... and read the look's own text bias instead (np-look34,
    // LookSettings::uiLodBias; a uniform, so a token moves it live).
    key.uiBias = MaterialDrawUiBias(isTextMesh || isOverlayPass, isTextMesh,
                                    gWgpuRnd->Pipelines().UiTakesLodBias());
    key.outputPixelSampling = MaterialDrawSamplesAtOutputPixels(
        isTextMesh || isOverlayPass, ctx.toTexture,
        gWgpuRnd->Pipelines().OutputPixelScale());
    return key;
}

PipelineKey MeshNextPassPipelineKey(const PipelineKey& first, BaseMaterial* nextPass,
                                    const MeshPassContext& ctx) {
    // Pipeline may differ (blend mode, z mode, etc.)
    PipelineKey npKey = first;
    npKey.blend = (WgpuBlend)rndshape::Mat(nextPass)->GetBlend();
    npKey.zMode = (WgpuZMode)rndshape::Mat(nextPass)->GetZMode();
    WgpuCull npCull = (WgpuCull)rndshape::Mat(nextPass)->GetCull();
    npCull = ReflectionCull(npCull, ctx.reflection);
    if (ctx.selfShadowCaster) npCull = first.cull;
    npKey.cull = npCull;
    npKey.stencil = (WgpuStencil)rndshape::Mat(nextPass)->GetStencil();
    npKey.alphaCut = rndshape::Mat(nextPass)->GetAlphaCut();
    npKey.alphaWrite = rndshape::Mat(nextPass)->GetAlphaWrite();
    if (rndshape::kRenderTargetForcesAlphaWrite && ctx.toTexture
        && !rndshape::MatPinsAlphaWrite(nextPass))
        npKey.alphaWrite = true;
    const CutCoverage npCutCov = MaterialCutCoverage(nextPass);
    npKey.alphaToCoverage = npCutCov == CutCoverage::AlphaToCoverage;
    npKey.cutoutCoverage = npCutCov == CutCoverage::Sharpened;
    return npKey;
}

namespace {
// Queue the keys `key` stands for once `v` is applied, and each NextPass's.
// A variant replaces the material state a property animation keys (blend,
// z mode, alpha write); the draw reads the material as it is that frame.
// The pseudo-HDR bloom mask (rndshape::BloomMaskScale) turns alphaWrite on
// for a material that allows it, in the main frame, but only while the frame
// has a current post-proc and no environment fade -- state of the frame, not
// of the content -- so such a material is warmed both ways in the main frame.
// A text or HUD key (textMesh, or an overlay pass, into the frame) is queued
// at each output-pixel scale a resize can give the frame
// (PipelineManager::OutputPixelScaleLo/Hi), so a window resized across the
// render-scale step draws its text with no pipeline made.
int WarmAtEachScale(PipelineManager& pm, PipelineKey key, bool uiOutput) {
    const float lo = pm.OutputPixelScaleLo(), hi = pm.OutputPixelScaleHi();
    if (!uiOutput || lo == hi) return pm.WarmAsync(key) ? 1 : 0;
    int queued = 0;
    for (float s : {lo, hi}) {
        key.outputPixelSampling = MaterialDrawSamplesAtOutputPixels(true, false, s);
        key.outputPixelScale = key.outputPixelSampling ? s : 1.0f;
        queued += pm.WarmAsync(key) ? 1 : 0;
    }
    return queued;
}

int WarmVariant(PipelineKey key, RndMat* rawMat, bool glyph, bool textMesh,
                const MeshPassContext& ctx, const MatKeyVariant& v) {
    PipelineManager& pm = gWgpuRnd->Pipelines();
    const bool uiOutput = (textMesh || !ctx.hasDepth) && !ctx.toTexture;
    auto mat = rndshape::Mat(rawMat);
    const int blend = v.blend >= 0 ? v.blend : (int)mat->GetBlend();
    const bool alphaWrite = v.alphaWrite >= 0 ? v.alphaWrite != 0 : (bool)mat->GetAlphaWrite();
    key.blend = (WgpuBlend)blend;
    if (v.zMode >= 0 && (!glyph || GlyphKeepsMatZMode())) key.zMode = (WgpuZMode)v.zMode;
    key.alphaWrite = alphaWrite;
    if (rndshape::kRenderTargetForcesAlphaWrite && ctx.toTexture
        && !rndshape::MatPinsAlphaWrite(rawMat))
        key.alphaWrite = true;
    int queued = WarmAtEachScale(pm, key, uiOutput);
    if (ctx.hasDepth && !ctx.overlay && !ctx.toTexture &&
        rndshape::AllowsBloomMaskFor(mat, blend, alphaWrite)) {
        PipelineKey masked = key;
        masked.alphaWrite = true;
        queued += WarmAtEachScale(pm, masked, uiOutput);
    }
    for (BaseMaterial* np = mat->NextPass(); np; np = rndshape::Mat(np)->NextPass())
        queued += WarmAtEachScale(pm, MeshNextPassPipelineKey(key, np, ctx), uiOutput);
    return queued;
}
} // namespace

int WarmMeshPipelines(RndMesh* mesh, const MeshPassContext& ctx, const MatKeyVariant* variants,
                      int numVariants) {
    if (!gWgpuRnd || !mesh) return 0;
    RndMat* rawMat = mesh->Mat();
    if (!rawMat) rawMat = rndshape::DefaultMat();
    auto mat = rndshape::Mat(rawMat);
    if (!mat || ShouldSkipMesh(mesh->Name(), mat)) return 0;
    const GpuMeshData* gd = GetMeshGpuData(mesh);
    const bool skinned = mesh->IsSkinned();
    const PipelineKey key = MeshPipelineKey(mesh, rawMat, skinned, gd ? gd->depthBias : 0, ctx);
    const bool glyph = MeshDrawsAsGlyph(mesh, skinned);
    const bool textMesh = MeshShadesAsText(mesh);
    int queued = WarmVariant(key, rawMat, glyph, textMesh, ctx, MatKeyVariant{});
    for (int i = 0; i < numVariants; ++i)
        queued += WarmVariant(key, rawMat, glyph, textMesh, ctx, variants[i]);
    return queued;
}

int WarmMaterialPipelines(RndMat* rawMat, bool skinned, const MeshPassContext& ctx,
                          const MatKeyVariant* variants, int numVariants) {
    if (!gWgpuRnd || !rawMat) return 0;
    auto mat = rndshape::Mat(rawMat);
    if (!mat || ShouldSkipMesh("", mat)) return 0;  // a runtime mesh: named at its first draw
    const PipelineKey key = MaterialPipelineKey(rawMat, skinned, false, false, 0, ctx);
    int queued = WarmVariant(key, rawMat, false, false, ctx, MatKeyVariant{});
    for (int i = 0; i < numVariants; ++i)
        queued += WarmVariant(key, rawMat, false, false, ctx, variants[i]);
    return queued;
}

int WarmTextPagePipelines(RndMat* fontMat, const MeshPassContext& ctx) {
    if (!gWgpuRnd || !fontMat) return 0;
    auto mat = rndshape::Mat(fontMat);
    if (!mat || ShouldSkipMesh("", mat)) return 0;  // a page is unnamed
    // A glyph page (MeshDrawsAsGlyph and MeshShadesAsText both hold for a page
    // the game marks) of an unskinned mesh, as FontMap::AllocateMeshes makes.
    const PipelineKey key = MaterialPipelineKey(fontMat, false, true, true, 0, ctx);
    return WarmVariant(key, fontMat, true, true, ctx, MatKeyVariant{});
}

void DrawMeshImmediate(RndMesh* mesh) {
    if (!gWgpuRnd || !gWgpuRnd->IsInPass()) return;

    bool capturing = FrameCapture::Get().IsCapturing();
    uint32_t heuristics = 0;

    // Re-upload scene uniforms if camera changed (e.g., UI camera vs world camera)
    gWgpuRnd->EnsureSceneUniformsCurrent();

    // DC3 BaseMaterial getters, whatever the rndobj shape (platform/rndshape/).
    // A material-less mesh draws with the renderer's default material, as the
    // image's RndShaderStandard::Select does (see RndMeshDrawShowingSkip).
    RndMat* rawMat = mesh->Mat();
    if (!rawMat) rawMat = rndshape::DefaultMat();
    auto mat = rndshape::Mat(rawMat);
    if (!mat) {
        if (capturing) FrameCapture::Get().AddSkip(MeshLabel(mesh), "no material");
        return;
    }

    if (ShouldSkipMesh(mesh->Name(), mat)) return;

    // Ensure mesh data is on GPU
    if (!EnsureMeshUploaded(mesh)) {
        if (capturing) FrameCapture::Get().AddSkip(MeshLabel(mesh), "upload failed");
        return;
    }

    auto& meshData = *GetMeshGpuData(mesh);
    auto& pass = gWgpuRnd->CurrentPass();
    bool skinned = meshData.skinned;

    // Text meshes (created by RndText::FontMap) have empty names (not registered in ObjectDir).
    // Debug labels are stored in GpuMeshData::debugLabel for GPU debugging.
    // Only a glyph page gives up its material's depth test and culling
    // (MeshDrawsAsGlyph: a page the game marked, or -- for a game that does not
    // mark them -- an unnamed unskinned mesh). Other code builds unnamed meshes
    // too: spotlight beams, RndRibbon/RndLine, crowd billboards, and DC3's
    // HamRibbon hand trail (skinned, hamribbon.mat asks zMode 2 / cull 1).
    // Drawn as text they went through whatever stood in front of them.
    const bool isGlyphMesh = MeshDrawsAsGlyph(mesh, skinned);
    // The text colour path (forced prelit, atlas alpha as RGB) follows the same
    // mark when the game sets one (MeshShadesAsText); otherwise every unnamed
    // mesh, as before. A ribbon shaded as text happened to look right only
    // because its vertices are white.
    bool isTextMesh = MeshShadesAsText(mesh);

    // --- Pipeline selection --- (MeshPipelineKey: the load-time warm asks
    // the same function, so it cannot warm a key this draw would not ask for)
    const MeshPassContext passCtx = CurrentMeshPassContext();
    const bool isOverlayPass = !passCtx.hasDepth;
    const PipelineKey key = MeshPipelineKey(mesh, rawMat, skinned, meshData.depthBias, passCtx);
    // RB3's pseudo-HDR bloom mask (rndshape::BloomMaskScale; 0 for DC3): an
    // AllowHDR material drawn into the main frame writes its luma to alpha
    // (MeshPipelineKey sets alphaWrite for it).
    const float bloomMaskScale = (!isOverlayPass && !passCtx.overlay && !passCtx.toTexture)
        ? rndshape::BloomMaskScale(mat) : 0.0f;

    wgpu::RenderPipeline pipeline = gWgpuRnd->Pipelines().GetPipeline(key);
    if (!pipeline) return;

    // --- Material (group 1) ---
    MaterialParams matParams = BuildMaterialParams(mat, isTextMesh);
    matParams.uniforms.bloomMaskScale = bloomMaskScale;
    // Text glyph colour. BuildMaterialParams routes a text mesh's atlas alpha
    // into RGB (useAlphaAsRGB), which is right for an atlas that keeps its
    // glyphs in alpha with RGB == 0 and throws away the colour of one that
    // does not. A game whose font atlases carry glyph colour in RGB says so
    // through QueryDrawMaterialPolicy().isColorIcon -- the same field, and the
    // same exclusion, as RB3MaterialBinder's colour-icon fonts. Without it
    // DC3's button glyphs (icons_buttons.mat: a coloured disc with a dark
    // letter) draw as blank white discs and its two checkbox glyphs ('a'/'b'
    // in icons.mat) as the same white square.
    if (isTextMesh && matParams.uniforms.useAlphaAsRGB != 0.0f &&
        TextMeshKeepsAtlasRGB(mesh, rawMat, skinned)) {
        matParams.uniforms.useAlphaAsRGB = 0.0f;
        matParams.heuristics &= ~(uint32_t)kHeuristicTextAlphaAsRGB;
    }
    heuristics |= matParams.heuristics;

    // A draw that cannot change a pixel is not sent (DrawChangesNothing). The
    // mesh is uploaded and its pipeline made above all the same, so the frame
    // it fades in on pays nothing new. A NextPass chain draws with this pass's
    // geometry bindings, so a material with one always draws.
    // MILO_DRAW_INVISIBLE=1 sends them as before.
    if (rndshape::kSkipInvisibleDraws && !mat->NextPass() &&
        DrawChangesNothing(key, matParams.uniforms.color[3], matParams.uniforms.bloomMaskScale)) {
        if (!DrawInvisibleMeshes()) {
            NoteInvisibleMeshDrawSkipped();
            if (capturing) FrameCapture::Get().AddSkip(MeshLabel(mesh), "alpha 0, writes nothing");
            return;
        }
    }

    pass.SetPipeline(pipeline);

    uint32_t matOffset = gWgpuRnd->MaterialRing().Write(
        gWgpuRnd->Gpu().Queue(), &matParams.uniforms, sizeof(matParams.uniforms));

    wgpu::Sampler sampler = gWgpuRnd->Gpu().GetSampler(matParams.samplerDesc);
    wgpu::Sampler mapSampler = gWgpuRnd->Gpu().GetSampler(matParams.mapSamplerDesc);

    // Groups 1-3 are cached by the resources they bind; this draw's uniform
    // blocks are reached through the dynamic offsets (PLAN H-30).
    wgpu::BindGroup matBG = gWgpuRnd->MaterialBindGroup(matParams.texViews, sampler, mapSampler);
    pass.SetBindGroup(1, matBG, 1, &matOffset);

    // --- Object uniforms (group 2) ---
    ObjectUniforms objUni{};
    if (skinned) {
        // Skinned: bone matrices already produce world-space positions,
        // so object transform must be identity to avoid double-transform
        Transform identity;
        identity.Reset();
        FillObjectUniforms(identity, objUni);
    } else {
        FillObjectUniforms(mesh->WorldXfm(), objUni);
    }
    // RB3's retail approx-light box map for this mesh (no-op for DC3).
    rndshape::FillMeshApproxLighting(mesh, objUni.boxLight, objUni.retail);

    uint32_t objOffset = gWgpuRnd->ObjectRing().Write(
        gWgpuRnd->Gpu().Queue(), &objUni, sizeof(objUni));

    wgpu::BindGroup objBG = gWgpuRnd->ObjectBindGroup();
    pass.SetBindGroup(2, objBG, 1, &objOffset);

    // --- Bone uniforms (group 3) ---
    uint64_t boneToken = 0;   // draw-log identity of the bone data bound
    if (skinned) {
        BoneUniforms boneUni{};
        FillBoneUniforms(mesh, boneUni);

        uint32_t boneOffset = gWgpuRnd->BoneRing().Write(
            gWgpuRnd->Gpu().Queue(), &boneUni, sizeof(boneUni));

        wgpu::BindGroup boneBG = gWgpuRnd->BoneBindGroup();
        pass.SetBindGroup(3, boneBG, 1, &boneOffset);
        if (rndshape::DrawLogActive())
            boneToken = DrawLogToken(gWgpuRnd->BoneRing().Buffer().Get(), boneOffset);
    } else {
        // Static mesh: bind dummy bone bind group (pipeline layout requires group 3)
        EnsureDummyBoneBindGroup();
        const uint32_t noOffset = 0;  // the dummy buffer holds one block
        pass.SetBindGroup(3, GetDummyBoneBindGroup(), 1, &noOffset);
        if (rndshape::DrawLogActive())
            boneToken = DrawLogToken(GetDummyBoneBindGroup().Get(), 0);
    }

    // --- Capture record ---
    if (capturing) {
        RecordDrawCall(mesh, mat, matParams, skinned, (int)mat->GetBlend(), heuristics);
        auto& rec = FrameCapture::Get().LastRecord();
        rec.hasPipeState = true;
        rec.pipeZMode = (int)key.zMode;
        rec.pipeCull = (int)key.cull;
        rec.passHasDepth = key.hasDepth;
        rec.glyph = isGlyphMesh;
        rec.uploadedVerts = meshData.numVertices;
        rec.uploadedTriangles = meshData.numIndices / 3;
        // geomOwner is set by UploadMesh; a GpuMeshData filled another way has none.
        rec.ownerVerts = meshData.geomOwner ? meshData.geomOwner->NumVerts() : 0;
        rec.ownerTriangles = meshData.geomOwner ? meshData.geomOwner->NumFaces() : 0;
    }

    // --- Draw ---
    size_t vertexSize = skinned ? sizeof(GpuVertexSkinned) : sizeof(GpuVertex);
    pass.SetVertexBuffer(0, meshData.vertexBuffer, 0, meshData.numVertices * vertexSize);
    pass.SetIndexBuffer(meshData.indexBuffer, wgpu::IndexFormat::Uint16, 0,
                        meshData.numIndices * sizeof(uint16_t));

    pass.DrawIndexed(meshData.numIndices);

    // --- Per-draw state log (RB3's RB3_DRAWLOG; off for DC3) ---
    if (rndshape::DrawLogActive()) {
        rndshape::DrawLogDraw d;
        d.pipelineHash = (uint64_t)PipelineKeyHash{}(key);
        d.blend = (uint8_t)(int)key.blend;
        d.zMode = (uint8_t)(int)key.zMode;
        d.layout = (uint8_t)(int)key.layout;
        d.hasDepth = key.hasDepth;
        d.alphaCut = key.alphaCut;
        d.alphaWrite = key.alphaWrite;
        d.skinned = skinned;
        d.targetFormat = (uint32_t)key.targetFormat;
        d.indexCount = meshData.numIndices;
        d.triCount = meshData.numIndices / 3;
        d.vertCount = meshData.numVertices;
        d.mesh = mesh;
        d.mat = rawMat;
        d.world = objUni.world;
        d.viewProj = gWgpuRnd->LastSceneViewProj();
        d.boundColor = matParams.uniforms.color;
        d.viewportW = (float)gWgpuRnd->Gpu().WindowWidth();
        d.viewportH = (float)gWgpuRnd->Gpu().WindowHeight();
        d.sceneToken = DrawLogToken(gWgpuRnd->SceneBuffer().Get(), gWgpuRnd->SceneOffset());
        d.matToken = DrawLogToken(gWgpuRnd->MaterialRing().Buffer().Get(), matOffset);
        d.objToken = DrawLogToken(gWgpuRnd->ObjectRing().Buffer().Get(), objOffset);
        d.boneToken = boneToken;
        rndshape::DrawLogRecord(d);
    }

    // --- Multi-pass materials ---
    // Walk the NextPass chain and draw additional passes with the same geometry
    BaseMaterial* nextPass = mat->NextPass();
    while (nextPass) {
        const PipelineKey npKey = MeshNextPassPipelineKey(key, nextPass, passCtx);

        wgpu::RenderPipeline npPipeline = gWgpuRnd->Pipelines().GetPipeline(npKey);
        if (npPipeline) {
            pass.SetPipeline(npPipeline);

            MaterialParams npParams = BuildPassMaterialParams(nextPass);

            uint32_t npMatOffset = gWgpuRnd->MaterialRing().Write(
                gWgpuRnd->Gpu().Queue(), &npParams.uniforms, sizeof(npParams.uniforms));

            wgpu::BindGroup npMatBG =
                gWgpuRnd->MaterialBindGroup(npParams.texViews, sampler, mapSampler);
            pass.SetBindGroup(1, npMatBG, 1, &npMatOffset);

            // Object + bone bind groups unchanged, just re-draw
            pass.DrawIndexed(meshData.numIndices);
        }
        nextPass = rndshape::Mat(nextPass)->NextPass();
    }

    // np-gpuprof2: MILO_GPU_SPLIT_DRAWS times each frame-pass draw on its own.
    if (WgpuRnd::TimingSplitDraws()) {
        std::string what = mesh->Name() ? mesh->Name() : "?";
        what += "/";
        what += rawMat && rawMat->Name() ? rawMat->Name() : "?";
        char tail[48];
        snprintf(tail, sizeof(tail), "/b%d/t%u", (int)key.blend, meshData.numIndices / 3);
        what += tail;
        gWgpuRnd->TimingSplitAfterDraw(what.c_str());
    }
}

// ============================================================================
// Shadow depth drawing — simplified path for shadow map generation
// ============================================================================

void DrawMeshShadow(RndMesh* mesh) {
    if (!gWgpuRnd || !gWgpuRnd->InShadowPass()) return;

    // Ensure mesh data is on GPU
    if (!EnsureMeshUploaded(mesh)) return;

    auto& meshData = *GetMeshGpuData(mesh);
    auto& pass = gWgpuRnd->ShadowRenderPass();
    bool skinned = meshData.skinned;

    // Select shadow pipeline
    if (skinned) {
        pass.SetPipeline(gWgpuRnd->ShadowSkinnedPipeline());
    } else {
        pass.SetPipeline(gWgpuRnd->ShadowStaticPipeline());
    }

    // Object uniforms (group 1) — world matrix
    ObjectUniforms objUni{};
    if (skinned) {
        Transform identity;
        identity.Reset();
        FillObjectUniforms(identity, objUni);
    } else {
        FillObjectUniforms(mesh->WorldXfm(), objUni);
    }

    uint32_t objOffset = gWgpuRnd->ObjectRing().Write(
        gWgpuRnd->Gpu().Queue(), &objUni, sizeof(objUni));

    // Object bind group on the shadow layout: one cached group over the ring,
    // this draw's block reached through the dynamic offset (PLAN H-30).
    {
        wgpu::BindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = gWgpuRnd->ObjectRing().Buffer();
        entry.offset = 0;
        entry.size = sizeof(ObjectUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = gWgpuRnd->ShadowObjectBGL();
        bgDesc.entryCount = 1;
        bgDesc.entries = &entry;
        wgpu::BindGroup objBG = gWgpuRnd->BindGroups().GetOrCreate(gWgpuRnd->Gpu().Device(), bgDesc);
        pass.SetBindGroup(1, objBG, 1, &objOffset);
    }

    // Bone uniforms (group 2) — only for skinned
    if (skinned) {
        BoneUniforms boneUni{};
        FillBoneUniforms(mesh, boneUni);

        uint32_t boneOffset = gWgpuRnd->BoneRing().Write(
            gWgpuRnd->Gpu().Queue(), &boneUni, sizeof(boneUni));

        wgpu::BindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = gWgpuRnd->BoneRing().Buffer();
        entry.offset = 0;
        entry.size = sizeof(BoneUniforms);

        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = gWgpuRnd->ShadowBoneBGL();
        bgDesc.entryCount = 1;
        bgDesc.entries = &entry;
        wgpu::BindGroup boneBG = gWgpuRnd->BindGroups().GetOrCreate(gWgpuRnd->Gpu().Device(), bgDesc);
        pass.SetBindGroup(2, boneBG, 1, &boneOffset);
    }

    // Draw
    size_t vertexSize = skinned ? sizeof(GpuVertexSkinned) : sizeof(GpuVertex);
    pass.SetVertexBuffer(0, meshData.vertexBuffer, 0, meshData.numVertices * vertexSize);
    pass.SetIndexBuffer(meshData.indexBuffer, wgpu::IndexFormat::Uint16, 0,
                        meshData.numIndices * sizeof(uint16_t));
    pass.DrawIndexed(meshData.numIndices);
}
