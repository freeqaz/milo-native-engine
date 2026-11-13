// RndShape_DC3.h — the DC3 (2012-era Xbox) rndobj shape: dc3-decomp, rb3-xenon.
//
// The dc3 backend was written against this shape, so every entry here is an
// identity forwarder onto the decomp's own accessor. Do not put behaviour here:
// a DC3/xenon render must not change because this header exists. Include via
// platform/rndshape/RndShape.h, never directly.
#pragma once

#include <type_traits>
#include <utility>

#include "rndobj/Rnd.h"
#include "rndobj/Rnd_NG.h"
#include "rndobj/BaseMaterial.h"
#include "rndobj/Mat.h"
#include "rndobj/Cam.h"
#include "rndobj/CubeTex.h"
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

// Whether a material ignores lighting entirely (register colour x texture, no
// ambient, lights or vertex colour). DC3 has no such material state.
template <class T>
inline bool MatUnlit(const T &) { return false; }
// Whether a prelit material's vertex colour is its ambient term (lights add
// on top). DC3's prelit materials skip lighting.
template <class T>
inline bool MatPrelitAmbient(const T &) { return false; }
// RB3's retail material terms (RndShape.h). DC3 materials keep the dc3
// shader's own specular/normal/rim model.
template <class T>
inline bool MatRetailTerms(const T &, RetailMatTerms &) { return false; }
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
// DC3 particle positions are already in world space, and DC3 draws particle
// colour as authored.
inline Vector3 PartWorldPos(RndParticleSys *, RndParticle *p) {
    return Vector3(p->pos.x, p->pos.y, p->pos.z);
}
constexpr bool kPartMaterialTint = false;

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
// DC3's RndTexRenderer::DrawToTexture leaves each material's alpha-write state
// alone while it draws into the output texture.
constexpr bool kRenderTargetForcesAlphaWrite = false;
// DC3 draws into render targets with the same sRGB-encoded output as the frame.
constexpr bool kRenderTargetStoresLinear = false;

// ---- 2D rects --------------------------------------------------------------
// DC3's DrawRect colours a rect by the colour argument alone.
constexpr bool kRectModulatesMatColor = false;

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
// DC3 and rb3-xenon grade through gfx/PostProcPass's own composite; the RB3
// retail chain (gfx/RB3RetailPost) never runs for them.
constexpr bool kRetailPostChain = false;
template <class Params>
inline void FillRetailPost(const RndPostProc *, float, Params &) {}
inline int RetailPostMode() { return 0; }
inline bool RetailBloomMaskActive() { return false; }
// Destination alpha blends with MAX, as retail: DxRnd::SetDefaultRenderStates
// (100% matched in dc3-decomp, Rnd_Xbox.cpp) sets BlendOpAlpha 3 once and
// nothing changes it; see dc3-backend-for-rb3-wii.md section 30. Measured on
// dc3-native frames 50-600 and the glitterati/dclive venues: RGB identical,
// frame alpha moves on 0.25% of dclive's pixels (PostProcPass reads RGB only).
constexpr bool kAlphaBlendMax = true;
// DC3 materials and lights are linear-space values (the dc3 shader's model).
constexpr bool kGammaSpaceShading = false;
template <class M>
inline float BloomMaskScale(const M &) { return 0.0f; }
// No display gamma ramp: the frame is presented as rendered.
inline float DisplayGamma(bool) { return 0.0f; }

// ---- scene lighting --------------------------------------------------------
// DC3 lights through WgpuRnd::WriteSceneUniforms' own environ block.
inline bool WriteSceneLighting(SceneUniforms &, RndCam *) { return false; }
inline void FillMeshApproxLighting(RndMesh *, float (*)[4], float *) {}

// ---- per-draw state log ----------------------------------------------------
// RB3's draw log (RB3DrawLogDebug.h). DC3 and rb3-xenon have no consumer for
// it, so it is off and every hook compiles away.
inline bool DrawLogActive() { return false; }
inline void DrawLogFrameBegin() {}
inline void DrawLogFrameEnd(int) {}
inline void DrawLogPassOpen(int) {}
inline void DrawLogRecord(const DrawLogDraw &) {}

} // namespace rndshape
