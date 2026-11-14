// RndShape_DC3.h — the DC3 (2012-era Xbox) rndobj shape: dc3-decomp, rb3-xenon.
//
// The dc3 backend was written against this shape, so every entry here is an
// identity forwarder onto the decomp's own accessor. Do not put behaviour here:
// a DC3/xenon render must not change because this header exists. Include via
// platform/rndshape/RndShape.h, never directly.
#pragma once

#include <limits>
#include <type_traits>
#include <utility>

#include "rndobj/Rnd.h"
#include "rndobj/Rnd_NG.h"
#include "rndobj/BaseMaterial.h"
#include "rndobj/Mat.h"
#include "rndobj/Cam.h"
#include "rndobj/CubeTex.h"
#include "rndobj/Draw.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/Mesh.h"
#include "rndobj/Part.h"
#include "rndobj/PostProc.h"
#include "math/Mtx.h"

#include <type_traits>
#include <utility>

// The NG renderer layer (NgRnd: viewport, Clear, DrawRect(ShaderType), and the
// RndShaderMgr constant cache) exists in this shape.
#define MILO_RNDOBJ_SHAPE_HAS_NGRND 1

// Rnd::PushClipPlanesInternal / PopClipPlanesInternal(ObjPtrVec<RndTransformable>&)
// are virtual hooks WgpuRnd overrides (RndDrawable::mClipPlanes): DC3's Rnd
// declares them for DxRnd, rb3-xenon's under HX_NATIVE (RND_DC3_VIRTUAL). The
// RB3-Wii shape has neither the hooks nor ObjPtrVec, so it stays undefined there.
#define MILO_RNDOBJ_SHAPE_HAS_CLIP_PLANES 1

struct RetailPostParams;   // gfx/RB3RetailPost.h (the DC3 retail post chain)

// WgpuRnd derives from the shape's renderer base.
using WgpuRndBase = NgRnd;

namespace rndshape {

// ---- renderer singleton (DC3 declares `Rnd& TheRnd`) ----------------------
inline Rnd &TheRndRef() { return TheRnd; }
// The renderer's default material (Rnd::Init: use_environ 0, pre_lit 1).
inline RndMat *DefaultMat() { return TheRnd.DefaultMat(); }

// ---- material --------------------------------------------------------------
// Mat(m) yields something whose operator-> exposes the DC3 BaseMaterial getter
// surface. In this shape that is simply the pointer.
template <class T>
inline T *Mat(T *m) { return m; }

// ---- which consumer -------------------------------------------------------
// Two consumers compile this shape: dc3-decomp and rb3-xenon. DC3's own light
// model (the image's NgEnviron / NgMat / RndShaderStandard / standard.vs+ps,
// docs/native/dc3-retail-lighting.md) is selected only for dc3-decomp, told
// apart by a structural fact of its rndobj: DC3's RndEnviron is a drawable
// (`class RndEnviron : public RndTransformable, public RndDrawable`, dc3-decomp
// src/system/rndobj/Env.h:14) and rb3-xenon's is a bare Hmx::Object
// (rb3-xenon src/system/rndobj/Env.h:47). rb3-xenon therefore keeps every
// answer below that it had before, and renders as it did.
//
// DependentEnvT<T>::type is RndEnviron, spelled so a branch that uses DC3-only
// members is not checked for the other consumer (see DependentMatT below).
template <class T> struct DependentEnvT { using type = RndEnviron; };
template <class T>
constexpr bool kDC3RetailModelFor =
    std::is_base_of<RndDrawable, typename DependentEnvT<T>::type>::value;
constexpr bool kDC3RetailModel = kDC3RetailModelFor<void>;

// Whether a material ignores lighting entirely (register colour x texture, no
// ambient, lights or vertex colour). DC3 (retail model): a material with
// use_environ off that is not prelit. RndShaderStandard::CalcShaderOpts gates
// both light bits (16 real, 17 approx) on mat->UseEnviron(), and
// NgMat::SetupAmbient puts (1,1,1,1) in c1 for such a material, so the shipped
// permutation 0x10 draws texel * c0 * 1 (100% matched; shaders section of the
// doc). rb3-xenon: no such state.
template <class T>
inline bool MatUnlit(const T &m) {
    if constexpr (kDC3RetailModelFor<T>) return m && !m->UseEnviron() && !m->Prelit();
    else { (void)m; return false; }
}
// Whether a prelit material's vertex colour is its ambient term (lights add
// on top). DC3 (retail model): prelit and use_environ, as permutation 0x20110
// computes (vertex * c1 + box) * c0 with c1 the environ's ambient. Prelit
// without use_environ (0x100 with c1 = 1) is the vertex colour as authored.
template <class T>
inline bool MatPrelitAmbient(const T &m) {
    if constexpr (kDC3RetailModelFor<T>) return m && m->Prelit() && m->UseEnviron();
    else { (void)m; return false; }
}
// The retail material terms (RndShape.h), from DC3's RndShaderStandard::
// CalcShaderOpts (100% matched) and NgMat::SetRegularShaderConst:
//   any of them only when the draw has a real or an approx light (the shader
//   reads that per draw, retailLightsOn) and the material uses the environ;
//   specular (bit 2) = SpecularRGB().Pack() != 0, c2 = (rgb, max(power, 0.5));
//   per-pixel (bit 0) = AllowPerPixel (1 on native, WgpuShaderMgr::Init) &&
//   GetPerPixelLit(); then normal map (5), specular map (1), rim (37:
//   RimRGB().Pack() != 0, c61 = (rgb, max(power, 0.5))), rim map (15);
//   c14 = 1 - deNormal.
// rb3-xenon keeps the dc3 shader's own model.
template <class T>
inline bool MatRetailTerms(const T &m, RetailMatTerms &t) {
    if constexpr (kDC3RetailModelFor<T>) {
        if (!m || !m->UseEnviron()) return false;
        const Hmx::Color &spec = m->GetSpecularRGB();
        t.specular = spec.Pack() != 0;
        if (t.specular) {
            t.specular_rgb[0] = spec.red;
            t.specular_rgb[1] = spec.green;
            t.specular_rgb[2] = spec.blue;
        }
        t.specularPower = spec.alpha - 0.5f >= 0.0f ? spec.alpha : 0.5f;
        t.perPixel = m->GetPerPixelLit();
        if (t.perPixel) {
            t.normalMap = m->NormalMap() != nullptr;
            t.normalTex = m->NormalMap();
            t.specularMap = t.specular && m->GetSpecularMap() != nullptr;
            t.specularTex = m->GetSpecularMap();
            const Hmx::Color &rim = m->GetRimRGB();
            t.rim = rim.Pack() != 0;
            if (t.rim) {
                t.rim_rgb[0] = rim.red;
                t.rim_rgb[1] = rim.green;
                t.rim_rgb[2] = rim.blue;
            }
            t.rimPower = rim.alpha - 0.5f >= 0.0f ? rim.alpha : 0.5f;
            t.rimLightUnder = t.rim && m->GetRimLightUnder();
            t.rimMap = t.rim && m->GetRimMap() != nullptr;
            t.rimTex = m->GetRimMap();
            t.deNormal = m->GetDeNormal();
        }
        return true;
    } else {
        (void)m;
        (void)t;
        return false;
    }
}
// Material colour modulation (RndShape.h; the shader's colorModScale /
// colorModAlpha). Returns the mode, 0 for none. DC3's RndMat carries it as
// mColorModFlags / mColorMod (rndobj/Mat.h): WorldCrowd::Draw3DChars sets
// kColorModModulate plus a crowd member's three random*.pal colours on the
// character's materials before each draw (world/Crowd.cpp SetMatColorFlags),
// and the Xbox shader permutation (key bits 32-33) multiplies them in.
//
// rb3-xenon compiles this shape too, and its RndMat has no GetColorMods()
// accessor: there (HasColorModAccessors false) the function keeps its old
// answer, 0, so an rb3-xenon render is unchanged.
} // namespace rndshape
#include <type_traits>
#include <utility>
namespace rndshape {
template <class M, class = void>
struct HasColorModAccessors : std::false_type {};
template <class M>
struct HasColorModAccessors<
    M, std::void_t<decltype(std::declval<const M &>().GetColorMods()),
                   decltype(std::declval<const M &>().GetColorModFlags())> >
    : std::true_type {};
// DependentMatT<T>::type is RndMat, spelled so the discarded branch below is not
// checked for a game whose RndMat lacks the accessors.
template <class T>
struct DependentMatT { using type = RndMat; };
template <class T>
inline int MatColorMod(T *m, float colours[3][4]) {
    using MatT = typename DependentMatT<T>::type;
    if constexpr (!HasColorModAccessors<MatT>::value) {
        (void)m;
        (void)colours;
        return 0;
    } else {
        MatT *mat = dynamic_cast<MatT *>(m);
        if (!mat) return 0;
        const int mode = mat->GetColorModFlags();
        if (mode == MatT::kColorModAlphaPack) return mode;
        if (mode != MatT::kColorModAlphaUnpackModulate && mode != MatT::kColorModModulate)
            return 0;
        const std::vector<Hmx::Color> &mods = mat->GetColorMods();
        if (mods.size() < 3) return 0;
        for (int i = 0; i < 3; i++) {
            colours[i][0] = mods[i].red;
            colours[i][1] = mods[i].green;
            colours[i][2] = mods[i].blue;
            colours[i][3] = 1.0f;
        }
        return mode;
    }
}
// Whether a colour-modulated material's texel alpha selects the tint band
// rather than carrying coverage (MaterialSetup FillColorMod -> colorMod.w bit
// 8 -> the shader skips its native alpha fringe fade, which would otherwise
// scale rgb by a / 0.1). True for DC3 kColorModModulate: the crowd characters'
// textures paint skin at texel alpha 0 -- band 0, tinted by the random1.pal
// skin tone -- on opaque kBlendSrc materials, and the fade blacked out every
// crowd face and hand.
template <class T>
inline bool ColorModAlphaIsBandSelector(const T &, int mode) {
    return mode == RndMat::kColorModModulate;
}
// World refraction (RndShape.h RefractTerms): the decomp's own RndMat
// accessors. GetRefractEnabled(true) skips its GetCurrentFrameTex test; the
// frame is bound whatever it holds (black before the first SavePreBuffer).
template <class T>
inline bool MatRefract(T *m, RefractTerms &r) {
    RndMat *mat = dynamic_cast<RndMat *>(m);
    if (!mat || !mat->GetRefractEnabled(true)) return false;
    r.strength = mat->GetRefractStrength();
    r.normalTex = mat->GetRefractNormalMap();
    return true;
}

// ---- environment -----------------------------------------------------------
inline RndEnviron *CurrentEnv() { return RndEnviron::Current(); }
inline bool EnvHasAmbientFogOwner(RndEnviron *e) { return e->AmbientFogOwner() != nullptr; }
inline float EnvFogStart(RndEnviron *e) { return e->FogStart(); }
inline float EnvFogEnd(RndEnviron *e) { return e->FogEnd(); }
inline ObjPtrList<RndLight> &EnvLightsApprox(RndEnviron *e) { return e->LightsApprox(); }
inline ObjPtrList<RndLight> &EnvLightsReal(RndEnviron *e) { return e->LightsReal(); }

// ---- light -----------------------------------------------------------------
inline RndTex *LightTexture(RndLight *l) { return l->GetTexture(); }
inline Transform LightProjection(RndLight *l) { return l->Projection(); }

// ---- camera ----------------------------------------------------------------
inline void CamViewProjectXfms(RndCam *c, Transform &view, Hmx::Matrix4 &proj) {
    c->GetViewProjectXfms(view, proj);
}
// A view-projection a tool set on the camera directly (milo-viewer's orbit
// cam). Identity == "not set"; the caller tests for that.
inline const Hmx::Matrix4 &CamViewProjMatrix(RndCam *c) { return c->GetViewProjMatrix(); }
inline const Vector2 &CamZRange(RndCam *c) { return c->ZRange(); }
inline const Hmx::Rect &CamScreenRect(RndCam *c) { return c->GetScreenRect(); }

// ---- mesh ------------------------------------------------------------------
inline RndMesh *MeshGeomOwner(RndMesh *m) { return m->GetGeomOwner(); }
inline int MeshNumCompressedVerts(RndMesh *m) { return (int)m->NumCompressedVerts(); }
inline unsigned char *MeshCompressedVerts(RndMesh *m) { return m->CompressedVerts(); }
// DxMesh::DrawShowing (826229B0; dc3-decomp src/system/rnddx9/Mesh.cpp, 100%
// matched) refills the geometry owner's vertex buffer before it draws when the
// verts were written through the property system since the last Sync:
//     if (geom->Verts().unkc) geom->Sync(0x1f);
// PropSync(RndMesh::VertVector) sets that flag on every `(verts N ...)` set, and
// RndMesh::Sync clears it. A PropAnim that keys vertex positions (Rehearse's
// projection_mesh.mesh reveal, world/shared/chars_base.milo) moves verts only
// this way; without the refill the GPU keeps the geometry of its first upload.
// rb3-xenon compiles this shape too and its VertVector has no such flag (its
// Mesh.h: retail VertVector is exactly 0xc), so the test is made only where the
// member exists; elsewhere this is a no-op, as before.
template <class M, class = void> struct MeshHasVertDirtyFlag : std::false_type {};
template <class M>
struct MeshHasVertDirtyFlag<M, std::void_t<decltype(std::declval<M &>().Verts().unkc)>>
    : std::true_type {};
template <class M> inline void MeshSyncDirtyVerts(M *geom) {
    if constexpr (MeshHasVertDirtyFlag<M>::value) {
        if (geom->Verts().unkc)
            geom->Sync(0x1f);
    }
}

inline void VertColor(const RndMesh::Vert &v, float out[4]) {
    out[0] = v.color.red;
    out[1] = v.color.green;
    out[2] = v.color.blue;
    out[3] = v.color.alpha;
}
inline const Vector2 &VertUV(const RndMesh::Vert &v) { return v.tex; }
inline void VertBoneWeights(const RndMesh::Vert &v, float out[4]) {
    out[0] = v.boneWeights.x;
    out[1] = v.boneWeights.y;
    out[2] = v.boneWeights.z;
    out[3] = v.boneWeights.w;
}

// ---- particles -------------------------------------------------------------
inline int PartTilesAcross(RndParticleSys *s) { return s->NumTilesAcross(); }
inline int PartTilesDown(RndParticleSys *s) { return s->NumTilesDown(); }
inline int PartTileIndex(const RndParticle *p) { return p->mCurrentTileIndex; }
// Particle positions live in the system's relative space, not world space:
// retail DxParticleSys::DrawShowing (dc3 src/system/rnddx9/Part.cpp, and
// rb3-xenon's identical one) draws them under
// TheShaderMgr.SetTransform(mRelativeXfm), with the billboard basis taken as
// Invert(mRelativeXfm.m) * camera so that, once transformed, the quad still
// faces the camera -- i.e. world corner = mRelativeXfm * pos + camera axes.
// mRelativeXfm is identity only while the motion parent has never moved: with
// a relative_motion in (0,1) UpdateRelativeXfm accumulates relative_motion *
// the parent's displacement, so a system placed after SetRelativeMotion ran
// (streetside's manholeSteam: relative_motion 0.8) stores its particles 80% of
// the way back toward the origin. Drawing p->pos raw put that steam column in
// the middle of the stage instead of on its manhole.
//
// A consumer whose RndParticleSys has no RelativeXfm() accessor keeps the old
// raw-position behaviour (rb3-xenon until it adds the accessor its retail
// code already implies).
template <typename Sys, typename = void>
struct PartHasRelativeXfm : std::false_type {};
template <typename Sys>
struct PartHasRelativeXfm<Sys, std::void_t<decltype(std::declval<const Sys &>().RelativeXfm())>>
    : std::true_type {};
template <typename Sys>
inline Vector3 PartWorldPos(Sys *s, RndParticle *p) {
    if constexpr (PartHasRelativeXfm<Sys>::value) {
        Vector3 rel(p->pos.x, p->pos.y, p->pos.z), w;
        Multiply(rel, s->RelativeXfm(), w);
        return w;
    } else {
        (void)s;
        return Vector3(p->pos.x, p->pos.y, p->pos.z);
    }
}
// No Wii-TEV tint model here: this shape scales particle colour the way its
// retail Xbox particle shader does (Part_Wgpu.cpp, ParticleShaderColor).
constexpr bool kPartMaterialTint = false;
// The inputs ParticleShaderColor reads: use_environ gates the environ ambient
// (NgMat::SetupAmbient), intensify doubles the texel (particles PS c5.y).
inline bool MatUseEnviron(RndMat *m) { return m->UseEnviron(); }
inline bool MatIntensify(RndMat *m) { return m->GetIntensify(); }

// ---- cube texture ----------------------------------------------------------
inline RndBitmap *CubeFaceBitmap(RndCubeTex *c, int face) {
    return &c->GetBitmap((RndCubeTex::CubeFace)face);
}

// ---- texture layout --------------------------------------------------------
// DC3 and rb3-xenon bitmaps are Xbox layouts (DXT big-endian words, Milo tiling
// via order & 4); none carries the Wii GX 0x40 order bit.
constexpr bool kGxTextureLayout = false;

// ---- draw modes ------------------------------------------------------------
// The Rnd::Mode WorldReflection::DrawShowing sets while it draws the mirrored
// world (DC3 world/Reflection.cpp: SetDrawMode((Rnd::Mode)8)).
constexpr int kDrawModeReflection = 8;

// ---- camera select ---------------------------------------------------------
// DC3's RndCam::Select ends by handing the renderer the camera's viewport --
// mScreenRect scaled to the target, depth range mZRange -- through
// TheNgRnd.SetViewport (rndobj/Cam.cpp), so WgpuRnd receives it on every select.
constexpr bool kCamSelectSetsViewport = true;

// ---- render-to-texture -----------------------------------------------------
// DC3 writes destination alpha for every material drawn into a render target.
// The image's colour-write mask is chosen per material select by
// SetColorWriteMask (dc3-decomp src/system/rndobj/Shader.cpp, 0x8269E770,
// 100% matched):
//     alpha = mForceAlphaWrite ? mAlphaWrite
//                              : (optAlpha || TheNgRnd.Offscreen() || mAlphaWrite)
// and Offscreen() (DxRnd::Offscreen, also 100% matched) is
// D3DDevice_GetRenderTarget(0) != BackBuffer(): true for ANY bound target.
// Nothing in the image's render-target or RndTexRenderer setup narrows this
// per target, so neither does this rule.
// So a target's alpha holds what was drawn into it unless the material pins
// its alpha write with force_alpha_write (MatPinsAlphaWrite below). rb3-xenon,
// which also compiles this shape, has the same rule without the exemption
// (its SetColorWriteMask, retail 0x824A5AF8: optAlpha || Offscreen() ||
// mAlphaWrite), and its material has no force_alpha_write member.
// Rehearse depends on it: the dancer is hidden by the practice camshots and
// shown only through world/shared/chars_base.milo's projection.rndtex ->
// projection.tex -> projection_mesh.mesh (projection.mat, SrcAlpha); with
// alpha writes off the target's alpha stayed at the clear value 0 and the
// dancer was invisible (dc3-decomp PLAN G-11).
constexpr bool kRenderTargetForcesAlphaWrite = true;
// force_alpha_write: the material's own alpha_write decides, offscreen or not.
template <class M, class = void> struct MatHasForceAlphaWrite : std::false_type {};
template <class M>
struct MatHasForceAlphaWrite<M, std::void_t<decltype(std::declval<const M &>().GetForceAlphaWrite())>>
    : std::true_type {};
template <class M> inline bool MatPinsAlphaWrite(const M *m) {
    if constexpr (MatHasForceAlphaWrite<M>::value)
        return m && m->GetForceAlphaWrite();
    else
        return false;
}
// DC3 draws into render targets with the same sRGB-encoded output as the frame.
constexpr bool kRenderTargetStoresLinear = false;

// ---- 2D rects --------------------------------------------------------------
// kRectModulatesMatColor is the RB3-Wii model (argument x material colour) and
// stays off here. DC3's (and rb3-xenon's) Xbox DrawRect is
// kRectXboxDrawRectShader: the colour overload of DxRnd::DrawRect selects
// kDrawRectShader, which RndShaderDrawRect::Select runs as the `standard`
// shader with c1 = white and c0 = the material colour (NgMat::SetupShader).
// Its vertex shader writes `mul o2, r1, c0` (vertex colour x c0) for a prelit
// material and `mul o2, c1, c0` (c0 alone) otherwise, and the pixel shader
// multiplies that by the texel. See DrawRectMaterialColor (gfx/DrawRect2D.h).
constexpr bool kRectModulatesMatColor = false;
constexpr bool kRectXboxDrawRectShader = true;
inline bool MatPrelit(RndMat *m) { return m->Prelit(); }

// ---- post-processing -------------------------------------------------------
// The grain strength the post-process shader adds per pixel, as authored. Only
// read when PostProcNoiseMap below cannot see the noise map (a consumer whose
// RndPostProc has no noise-map accessors, today rb3-xenon).
inline float PostProcGrain(const RndPostProc *pp) { return pp->GetNoiseIntensity(); }

// The image's post-process noise (NgPostProc::CheckNoise + postprocess.ps option
// bit 0x4, and 0x800000000000 for the midtone form; disassembled from the
// shipped xbox_shaders): a TEXTURE, mNoiseMap, sampled twice at
// (uv + seed) * mNoiseBaseScale (the second also * mNoiseTopScale, or * 1 when
// stationary), the two samples multiplied and overlay-blended into the scene at
// mNoiseIntensity. CheckNoise turns it off when the map is null. Filled only
// when the consumer's RndPostProc exposes GetNoiseMap/GetNoiseBaseScale/
// GetNoiseTopScale; otherwise returns false and PostProcPass keeps its
// procedural grain from PostProcGrain.
struct PostProcNoise {
    RndTex *map = nullptr;
    float baseScale[2] = {0.0f, 0.0f};
    float topScale = 1.0f;
    float intensity = 0.0f;
    bool stationary = false;
    bool midtone = false;
};
template <class P, class = void> struct HasPostProcNoiseMap : std::false_type {};
template <class P>
struct HasPostProcNoiseMap<
    P, std::void_t<decltype(std::declval<const P &>().GetNoiseMap()),
                   decltype(std::declval<const P &>().GetNoiseBaseScale()),
                   decltype(std::declval<const P &>().GetNoiseTopScale())>>
    : std::true_type {};
template <class P> inline bool PostProcNoiseMap(const P *pp, PostProcNoise &out) {
    if constexpr (HasPostProcNoiseMap<P>::value) {
        out.map = pp->GetNoiseMap();
        const auto &base = pp->GetNoiseBaseScale();
        out.baseScale[0] = base.x;
        out.baseScale[1] = base.y;
        out.topScale = pp->GetNoiseTopScale();
        out.intensity = pp->GetNoiseIntensity();
        out.stationary = pp->GetNoiseStationary();
        out.midtone = pp->GetNoiseMidtone();
        return true;
    } else {
        (void)pp;
        (void)out;
        return false;
    }
}
// ---- np-gfx-post: DC3 retail post chain --------------------------------------
// DC3 grades through its retail Xbox 360 post chain (NgPostProc::DoPost, ported
// from dc3-decomp rndobj/PostProc_NG.cpp and the shipped postprocess.ps / bloom.ps
// / blur.ps microcode; see gfx/RB3RetailPost.h, "DC3"): the per-material
// pseudo-HDR luma mask in the frame's alpha, three quarter-size bloom sets (or
// the two streak sets) screen-blended at bloomColor * bloom_loc *
// BloomIntensity(), the texture noise, the RndColorXfm matrix, then the
// vignette. rb3-xenon shares this shape but not the chain: the chain keys on
// RndPostProc::BloomLocFactor() and RndMat::GetBloomMultiplier(), HX_NATIVE
// accessors only dc3-decomp declares, so rb3-xenon keeps PostProcPass's own
// composite unchanged. RetailPostParams is gfx/RB3RetailPost.h's.
template <class P, class = void> struct PostProcHasBloomLocFactor : std::false_type {};
template <class P>
struct PostProcHasBloomLocFactor<P, std::void_t<decltype(P::BloomLocFactor()),
                                                decltype(std::declval<const P &>().GetColorModulation())>>
    : std::true_type {};
template <class M, class = void> struct MatHasBloomMultiplier : std::false_type {};
template <class M>
struct MatHasBloomMultiplier<M, std::void_t<decltype(std::declval<const M &>().GetBloomMultiplier())>>
    : std::true_type {};
constexpr bool kDC3RetailPost =
    PostProcHasBloomLocFactor<RndPostProc>::value && MatHasBloomMultiplier<RndMat>::value;
// The RB3 chain's EndWorld flush (WgpuRnd::FlushWorldPost) stays RB3's: DC3
// already grades at EndWorld through NgRnd::DoPostProcess.
constexpr bool kRetailPostChain = false;
// DC3_RETAIL_POST (rndshape/DC3PostChain.cpp): unset or anything else = 1 (the
// retail chain), "0" = 0 (PostProcPass's previous composite), "raw" = 2 (the
// scene ungraded), "mask" = 3 / "bloom" = 4 (the chain showing the bloom mask /
// the bloom term; for inspection), "grade" = 5 (the chain without bloom).
// Always 0 without kDC3RetailPost (rb3-xenon).
int RetailPostMode();
// The frame's alpha carries the bloom mask this frame.
inline bool RetailBloomMaskActive() {
    if constexpr (!kDC3RetailPost) {
        return false;
    } else {
        const int m = RetailPostMode();
        return m == 1 || m == 3 || m == 4;
    }
}
// The post-process Rnd::DoPostProcess runs and Rnd::BeginDrawing takes the
// bloom colour from: the blacklight override (set_postproc_blacklight_override,
// the voice-control letterbox's PostProc_Blacklight.pp), else the override
// (set_postproc_override), else RndPostProc::Current(). dc3-decomp's Rnd
// carries the HX_NATIVE accessor; rb3-xenon (no blacklight field, and an
// override typed PostProcessor*) gets Current() (DC3PostChain.cpp).
RndPostProc *ActivePostProc();
// Fills the chain's parameters from the current RndPostProc (DC3PostChain.cpp).
void FillRetailPost(const RndPostProc *pp, float flickerMul, ::RetailPostParams &out);
// SetBloomColor's c7 scale: 1/threshold above 1, else 1 (DC3PostChain.cpp).
float BloomLumaScale(const RndPostProc *pp);
// ---- np-gfx-post: destination alpha (dc3-backend-for-rb3-wii.md 31.5) ----
// Whether a blended draw's destination alpha takes MAX(src.a, dst.a)
// (PipelineManager::SetAlphaBlendMax) or follows the draw's own blend
// equation, the factors NgMat::SetBasicState passes for alpha too.
// DxRnd::SetDefaultRenderStates (100% matched) sets BlendOpAlpha 3, which
// section 30 read as MAX. Section 30.7 flipped the DC3 shape to MAX without a
// retail comparison, because nothing on DC3 read the frame alpha then. The
// retail post chain reads it as the bloom mask, and retail hardware footage
// of the menus refutes MAX for DC3: under MAX, the song-select "debloom"
// quads (Multiply, white, AllowHDR) raise the mask to 1 under the cards and the
// full-screen Multiply tint raises it to ~0.9 over the background, so the
// cards burn white and the background hazes (bloom term 0.4 at a background
// pixel that retail shows at 0.04). With alpha following the equation
// (Multiply: dst.a * src.a) the cards are grey and readable and the background
// stays dark, as on retail. dc3-decomp (kDC3RetailPost) only; rb3-xenon keeps
// MAX unchanged.
constexpr bool kAlphaBlendMax = !kDC3RetailPost;
// ---- end np-gfx-post ----
// DC3 shades in gamma space, as the image does: no bitmap format DxRnd uses
// carries a gamma sign (rnddx9/Rnd.cpp D3DFormatForBitmap: DXT1/3/5, DXN,
// A8R8G8B8, A1R5G5B5, X8R8G8B8), the frame is a D3DFMT_A8R8G8B8 EDRAM surface
// (Rnd_Xbox.cpp), and no standard.ps permutation decodes or encodes, so the
// shader multiplies texel, material colour and lighting as stored and the
// target saturates the product (material.gammaShading). rb3-xenon keeps the
// linear model with the sRGB output encode.
constexpr bool kGammaSpaceShading = kDC3RetailModel;
// The pseudo-HDR bloom mask (ShaderOptions bit 22): retail DC3's standard.ps /
// particles.ps / multimesh write alpha = sat(dot(rgb, c7) * c5.z) for a
// material whose NgMat::AllowHDR() holds, drawn into the main frame
// (RndShader*::CalcShaderOpts: !fadeOut && !Offscreen() && AllowHDR()), with
// c7 = (0.3, 0.59, 0.11) * BloomLumaScale and c5.z = mBloomMultiplier
// (NgMat::SetRegularShaderConst); RndShader::SetColorWriteMask leaves alpha
// alone for every other material, and the frame clears it to 0. Returned
// NEGATIVE, -(c7 scale * c5.z): the standard shader then takes the luma of the
// colour before the target clamps it (retail's rgb is unclamped), where RB3's
// positive scale keeps its clamped-colour mask. Rects and particles use the
// magnitude. 0 = no mask (not AllowHDR, or the chain is off). A material with
// mBloomMultiplier 0 still writes the mask, which is 0 (retail's c5.z = 0):
// it returns -FLT_MIN, a scale that keeps the mask write on and rounds the mask
// to 0. That is how DC3's "debloom" quads work: game_mode_icon's debloom.mat
// (Multiply, white, bloom_multiplier 0) zeroes the icon's mask under the
// draw's alpha equation (dst.a * 0), so the mode-select icon does not burn
// (dc3-backend-for-rb3-wii.md 31.6).
//
// The mask also needs the environ not to fade out: RndShaderStandard,
// RndShaderParticles, RndShaderMultimesh and RndShaderSyncTrack::CalcShaderOpts
// (dc3-decomp rndobj/Shader.cpp) set mPseudoHDR only when !fadeOut, with
// fadeOut = env->FadeOut() && env->FadeEnd() != env->FadeStart() for the
// current environ. RndShaderDrawRect::CalcShaderOpts has no such test, so a
// rect passes envFadeOutCancels = false.
template <class M>
inline float BloomMaskScale(const M &m, bool envFadeOutCancels = true) {
    using Raw = std::remove_cv_t<std::remove_pointer_t<std::decay_t<M>>>;
    if constexpr (!kDC3RetailPost || !MatHasBloomMultiplier<Raw>::value) {
        (void)m;
        return 0.0f;
    } else {
        if (!m || !RetailBloomMaskActive()) return 0.0f;
        const RndPostProc *pp = ActivePostProc();
        if (!pp) return 0.0f;
        if (envFadeOutCancels) {
            const RndEnviron *env = RndEnviron::Current();
            if (env && env->FadeOut() && env->FadeEnd() != env->FadeStart()) return 0.0f;
        }
        // NgMat::AllowHDR (dc3-decomp rndobj/Mat_NG.cpp).
        const auto b = m->GetBlend();
        if (b == Raw::kBlendSrcAlpha || b == Raw::kBlendSrcAlphaAdd || b == Raw::kPreMultAlpha ||
            m->GetAlphaCut() || m->GetAlphaWrite())
            return 0.0f;
        const float scale = -(BloomLumaScale(pp) * m->GetBloomMultiplier());
        return scale < 0.0f ? scale : -std::numeric_limits<float>::min();
    }
}
// The display gamma ramp DxRnd::SetupGamma loads (ramp[i] = pow(i/256, g),
// g from the system config's (rnd (gamma g)): DC3's default.dta sets 0.85).
// DC3 applies it to EVERY output, headless included, so screenshots and
// look_capture frames are post-ramp, i.e. what the TV shows
// (DC3_DISPLAY_GAMMA=off for the pre-ramp front buffer, or a value to
// override). rb3-xenon: 0, no ramp. See DC3PostChain.cpp.
float DisplayGamma(bool presenting);
// ---- end np-gfx-post ----

// ---- scene lighting --------------------------------------------------------
// DC3 (retail model): the image's NgEnviron::Select register set, see
// DC3SceneLighting.cpp and docs/native/dc3-retail-lighting.md. rb3-xenon:
// returns false / does nothing, so WgpuRnd::WriteSceneUniforms keeps its own
// environ block for it.
bool WriteSceneLighting(SceneUniforms &s, RndCam *cam);
void FillMeshApproxLighting(RndMesh *mesh, float box[6][4], float retail[4]);
// The vertex shader constant NgEnviron last uploaded to register `reg`
// (WgpuShaderMgr keeps Vector4 vertex constants). Defined in Rnd_Wgpu.cpp.
const float *CapturedVConstant(int reg);

// ---- shadows ---------------------------------------------------------------
// DC3 (retail model): the image has no generic shadow map. Its dancer floor
// shadows are a projected light with projected_blend 1 (shadow_projected):
// NgLight::CheckShadowMap draws the light's shadow objects into a 256x256
// coverage target from the light's cone, and every material with
// recv_proj_lights darkens its approx-light term by
// 1 - 0.75 * coverage * colour * sat(c65 . N) (standard.ps 0x100010030011).
// The native form: WriteSceneLighting finds that light (as NgEnviron::Select
// does) and puts its cone in SceneUniforms.lightViewProj with
// shadowEnabled = 2; WgpuRnd::EndDrawing then calls CollectProjShadowCasters
// and draws the casters into ShadowPass's map, which the next frame samples.
// rb3-xenon keeps the generic pre-pass. Defined in DC3SceneLighting.cpp.
constexpr bool kGenericShadowPass = !kDC3RetailModel;

// Rnd::BeginDrawing selects mDefaultEnv every frame (rndobj/Rnd.cpp), so a
// draw that selects no environ of its own -- the UI after EndWorld -- gets the
// default environ. WgpuRnd::BeginDrawing used to select it only when none was
// current, which left the last frame's environ (a venue's) current. DC3
// follows the image; rb3-xenon keeps the native rule (its renders are pinned
// byte-identical).
constexpr bool kSelectDefaultEnvEachFrame = kDC3RetailModel;
bool CollectProjShadowCasters(ProjShadowCasters &out);
// The projected light's gobo for the scene WriteSceneLighting just wrote
// (projected_blend 0: tf10, added as (1 - coverage) gobo c69 sat(c65 . N),
// standard.ps 0x10030011); null otherwise, and always null for rb3-xenon.
RndTex *SceneProjTexture();

// ---- per-draw state log ----------------------------------------------------
// RB3's draw log (RB3DrawLogDebug.h). DC3 and rb3-xenon have no consumer for
// it, so it is off and every hook compiles away.
inline bool DrawLogActive() { return false; }
inline void DrawLogFrameBegin() {}
inline void DrawLogFrameEnd(int) {}
inline void DrawLogPassOpen(int) {}
inline void DrawLogRecord(const DrawLogDraw &) {}

} // namespace rndshape
