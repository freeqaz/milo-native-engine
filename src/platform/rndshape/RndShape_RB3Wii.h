// RndShape_RB3Wii.h — the RB3-Wii (2010-era) rndobj shape, as compiled by
// ../rb3's native build (MILO_ENGINE_RNDOBJ_SHAPE=rb3wii).
//
// The dc3 backend names DC3 accessors. RB3-Wii holds the same data under older
// names, or not at all (Wii RB3 has no NgRnd, no BaseMaterial, no RndShaderMgr,
// no specular / normal / rim / environment maps on RndMat). Each entry below maps
// one DC3 accessor onto the Wii member that carries the same data, or returns
// the DC3 class's own default when the Wii class has no such datum. Every
// default is the value a freshly-constructed DC3 object would report, so a Wii
// material renders as a DC3 material with those features switched off.
//
// Include via platform/rndshape/RndShape.h, never directly.
#pragma once

#include "rndobj/Rnd.h"
#include "rndobj/Mat.h"
#include "rndobj/Cam.h"
#include "rndobj/CubeTex.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/Mesh.h"
#include "rndobj/Part.h"
#include "rndobj/PostProc.h"
#include "math/Mtx.h"
#include "math/Vec.h"
#include "math/Geo.h"

#include <cmath>

// ---------------------------------------------------------------------------
// Names DC3 declares that RB3-Wii does not
// ---------------------------------------------------------------------------

// DC3 hoists RndMat's state into a BaseMaterial base. RB3-Wii keeps it all on
// RndMat, with the same Blend/ZMode enumerators (same names, same values), so
// `BaseMaterial::kBlendSrcAlpha` and `BaseMaterial *` mean the same here.
using BaseMaterial = RndMat;

// DC3's ZMode is a namespace-scope enum; RB3-Wii's is RndMat::ZMode. Same
// enumerators, same values (Mat.h: Disable 0, Normal 1, Transparent 2, Force 3,
// Decal 4).
using ZMode = RndMat::ZMode;
constexpr RndMat::ZMode kZModeDisable = RndMat::kZModeDisable;
constexpr RndMat::ZMode kZModeNormal = RndMat::kZModeNormal;
constexpr RndMat::ZMode kZModeTransparent = RndMat::kZModeTransparent;
constexpr RndMat::ZMode kZModeForce = RndMat::kZModeForce;
constexpr RndMat::ZMode kZModeDecal = RndMat::kZModeDecal;

// DC3's three-state cull. RB3-Wii stores a single `bool mCull` (true = cull
// back faces, which is DC3's kCullRegular).
enum Cull {
    kCullNone = 0,
    kCullRegular = 1,
    kCullBackwards = 2
};

// DC3's shader-type enum (rndobj/ShaderOptions.h). The backend only ever
// names the standard shader, and only to tag a 2D rect draw.
enum ShaderType {
    kStandardShader = 18
};

// DC3's 4x4 matrix (math/Mtx.h). RB3-Wii has Vector4 but no Matrix4; the dc3
// backend only uses it as a projection-matrix value type.
namespace Hmx {
    class Matrix4 {
    public:
        Matrix4() {}
        Matrix4 &Zero() {
            x.Set(0, 0, 0, 0);
            y.Set(0, 0, 0, 0);
            z.Set(0, 0, 0, 0);
            w.Set(0, 0, 0, 0);
            return *this;
        }
        Matrix4 &Identity() {
            x.Set(1, 0, 0, 0);
            y.Set(0, 1, 0, 0);
            z.Set(0, 0, 1, 0);
            w.Set(0, 0, 0, 1);
            return *this;
        }
        Vector4 x;
        Vector4 y;
        Vector4 z;
        Vector4 w;
    };
}

// The NG renderer layer is absent: no NgRnd, no RndShaderMgr constant cache.
// MILO_RNDOBJ_SHAPE_HAS_NGRND stays undefined.

// WgpuRnd's base for this shape: RB3-Wii's Rnd plus the four NgRnd virtuals the
// dc3 backend overrides (viewport, Clear, the ShaderType DrawRect). Nothing in
// the Wii fork calls these; they exist so WgpuRnd keeps one definition.
class WgpuRndBase : public Rnd {
public:
    struct Viewport {
        Viewport() : X(0), Y(0), Width(0), Height(0), MinZ(0), MaxZ(0) {}
        unsigned int X;
        unsigned int Y;
        unsigned int Width;
        unsigned int Height;
        float MinZ;
        float MaxZ;
    };

    virtual void SetViewport(const Viewport &v) { mViewport = v; }
    virtual const Viewport &GetViewport() const { return mViewport; }
    virtual void Clear(unsigned int, const Hmx::Color &) {}
    using Rnd::DrawRect;
    virtual void DrawRect(
        const Hmx::Rect &, RndMat *, ShaderType, const Hmx::Color &, const Hmx::Color *,
        const Hmx::Color *
    ) {}

    Viewport mViewport;
};

namespace rndshape {

// ---- renderer singleton (RB3-Wii declares `Rnd* TheRnd`) -------------------
inline Rnd &TheRndRef() { return *TheRnd; }

// ---- material --------------------------------------------------------------
// The DC3 BaseMaterial getter surface over an RB3-Wii RndMat.
class MatView {
public:
    explicit MatView(RndMat *m) : mMat(m) {}
    const MatView *operator->() const { return this; }
    operator RndMat *() const { return mMat; }
    RndMat *Raw() const { return mMat; }
    const char *Name() const { return mMat->Name(); }

    // Same datum, same name on both shapes.
    RndMat::Blend GetBlend() const { return mMat->GetBlend(); }
    RndMat::ZMode GetZMode() const { return mMat->GetZMode(); }
    TexWrap GetTexWrap() const { return mMat->GetTexWrap(); }
    RndTex *GetDiffuseTex() const { return mMat->GetDiffuseTex(); }
    const Hmx::Color &GetColor() const { return mMat->GetColor(); }
    const Transform &TexXfm() const { return mMat->TexXfm(); }
    RndMat *NextPass() const { return mMat->NextPass(); }

    // Same datum, Wii member (no accessor in the Wii class).
    bool GetAlphaCut() const { return mMat->mAlphaCut; }
    int GetAlphaThreshold() const { return mMat->mAlphaThresh; }
    bool GetAlphaWrite() const { return mMat->mAlphaWrite; }
    Cull GetCull() const { return mMat->mCull ? kCullRegular : kCullNone; }
    StencilMode GetStencil() const { return mMat->mStencilMode; }
    TexGen GetTexGen() const { return mMat->mTexGen; }
    bool Prelit() const { return mMat->mPreLit; }
    bool GetFog() const { return mMat->mFog; }
    bool GetIntensify() const { return mMat->mIntensify; }
    ShaderVariation GetShaderVariation() const { return mMat->mShaderVariation; }
    RndTex *GetEmissiveMap() const { return mMat->mEmissiveMap; }
    float GetEmissiveMultiplier() const { return mMat->mEmissiveMultiplier; }

    // DC3-only material features. RB3-Wii materials carry none of them, so
    // report what a freshly-constructed DC3 BaseMaterial reports.
    RndTex *NormalMap() const { return nullptr; }
    RndTex *GetSpecularMap() const { return nullptr; }
    RndTex *GetRimMap() const { return nullptr; }
    RndTex *GetNormDetailMap() const { return nullptr; }
    RndCubeTex *GetEnvironMap() const { return nullptr; }
    const Hmx::Color &GetSpecularRGB() const { return sBlack; }
    const Hmx::Color &GetSpecular2RGB() const { return sBlack; }
    const Hmx::Color &GetRimRGB() const { return sBlack; }
    bool GetRimLightUnder() const { return false; }
    float GetDeNormal() const { return 0.0f; }
    float GetAnisotropy() const { return 0.0f; }
    float GetNormDetailTiling() const { return 1.0f; }
    float GetNormDetailStrength() const { return 0.0f; }
    bool GetEnvironMapFalloff() const { return false; }
    bool GetEnvironMapSpecMask() const { return false; }

private:
    RndMat *mMat;
    static inline const Hmx::Color sBlack = Hmx::Color(0, 0, 0, 0);
};

inline MatView Mat(RndMat *m) { return MatView(m); }
inline MatView Mat(const MatView &m) { return m; }

// ---- environment -----------------------------------------------------------
inline RndEnviron *CurrentEnv() { return RndEnviron::sCurrent; }
inline bool EnvHasAmbientFogOwner(RndEnviron *e) { return e->mAmbientFogOwner.Ptr() != nullptr; }
inline float EnvFogStart(RndEnviron *e) { return e->GetFogStart(); }
inline float EnvFogEnd(RndEnviron *e) { return e->GetFogEnd(); }
inline ObjPtrList<RndLight> &EnvLightsApprox(RndEnviron *e) { return e->mLightsApprox; }
inline ObjPtrList<RndLight> &EnvLightsReal(RndEnviron *e) { return e->mLightsReal; }

// ---- light -----------------------------------------------------------------
inline RndTex *LightTexture(RndLight *l) { return l->mTexture; }
// DC3's RndLight::Projection(): the spot-light texture projection built from
// the light's world frame, range and top/bottom radii, then the authored
// mTextureXfm, then the [-1,1] -> [0,1] bias. RB3-Wii's RndLight carries every
// input (mRange, mTopRadius, mBotRadius, mTextureXfm) but no such method; this
// is DC3's body (rndobj/Lit.cpp) over those members.
inline Transform LightProjection(RndLight *l) {
    if (l->mRange == 0.0f) {
        Transform ident;
        ident.Reset();
        return ident;
    }
    const Transform &w = l->WorldXfm();
    Vector3 xRow = w.m.x;
    float nzx = -w.m.z.x;
    float nzy = -w.m.z.y;
    float nzz = -w.m.z.z;
    Vector3 yRow = w.m.y;
    Vector3 pos = w.v;
    float topR = l->mTopRadius;
    float slope = (l->mBotRadius - topR) / l->mRange;

    Transform result;
    result.m.x.y = nzx;
    result.m.y.z = yRow.y * slope;
    result.m.z.z = yRow.z * slope;
    result.m.x.z = yRow.x * slope;
    result.v.x = -(pos.x * xRow.x + pos.y * xRow.y + pos.z * xRow.z);
    result.v.y = -(pos.x * nzx + pos.y * nzy + pos.z * nzz);
    result.v.z =
        topR - (pos.x * yRow.x * slope + pos.y * yRow.y * slope + pos.z * yRow.z * slope);
    result.m.x.x = xRow.x;
    result.m.y.x = xRow.y;
    result.m.z.x = xRow.z;
    result.m.y.y = nzy;
    result.m.z.y = nzz;

    // Separate outputs: RB3-Wii's Multiply(Transform, Transform, Transform) is
    // not documented alias-safe, DC3's is; the product is the same either way.
    Transform withTex;
    Multiply(result, l->mTextureXfm, withTex);
    static const Transform sBias(
        Hmx::Matrix3(0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f),
        Vector3(0.0f, 0.0f, 0.0f)
    );
    Multiply(withTex, sBias, result);
    return result;
}

// ---- camera ----------------------------------------------------------------
// DC3's RndCam::GetViewProjectXfms, ported onto RB3-Wii's members. The Wii
// camera computes mInvWorldXfm and mLocalProjectXfm exactly as DC3's does
// (RndCam::UpdateLocal / UpdatedWorldXfm are line-for-line the same), so the
// D3D-style projection DC3 builds from them is correct for Wii cameras too.
// Omitted relative to DC3: the HiResScreen tile rect and the NativeSettings
// camera-tuning offsets, neither of which exists in RB3-Wii.
inline void CamViewProjectXfms(RndCam *c, Transform &viewXfm, Hmx::Matrix4 &projMtx) {
    static const Transform sFlipYZ(
        Hmx::Matrix3(1, 0, 0, 0, 0, 1, 0, 1, 0), Vector3(0, 0, 0)
    );
    Multiply(c->mInvWorldXfm, sFlipYZ, viewXfm);

    projMtx.Zero();
    float nearPlane = c->mNearPlane;
    float farPlane = c->mFarPlane;
    float farRatio;
    if (c->mYFov == 0) {
        projMtx.w.w = 1.0f;
        farRatio = 1.0f / (farPlane - nearPlane);
    } else {
        projMtx.z.w = 1.0f;
        farRatio = farPlane / (farPlane - nearPlane);
    }

    const Hmx::Rect &sr = c->mScreenRect;
    float cx = sr.w * 0.5f + sr.x;
    float cy = sr.h * 0.5f + sr.y;
    float left = sr.x > 0.0f ? sr.x : 0.0f;
    float bottom = sr.y > 0.0f ? sr.y : 0.0f;
    float right = (sr.x + sr.w) < 1.0f ? (sr.x + sr.w) : 1.0f;
    float top = (sr.y + sr.h) < 1.0f ? (sr.y + sr.h) : 1.0f;

    float sx = sr.w * c->mLocalProjectXfm.m.x.x;
    float sy = -(sr.h * c->mLocalProjectXfm.m.z.y);
    float l = (left - cx) * 2.0f;
    float b = (bottom - cy) * 2.0f;
    float r = (right - cx) * 2.0f;
    float t = (top - cy) * 2.0f;

    projMtx.z.z = farRatio;
    projMtx.x.x = (sx * 2.0f) / (r - l);
    projMtx.y.y = (sy * 2.0f) / (t - b);
    projMtx.z.y = (t + b) / (t - b);
    projMtx.z.x = -((r + l) / (r - l));
    projMtx.w.z = -(nearPlane * farRatio);
}

// DC3 cameras can carry a view-projection a tool set directly (milo-viewer's
// orbit cam); identity means "not set". RB3-Wii cameras have no such field, so
// it is always unset.
inline const Hmx::Matrix4 &CamViewProjMatrix(RndCam *) {
    static const Hmx::Matrix4 sIdentity = Hmx::Matrix4().Identity();
    return sIdentity;
}
inline const Vector2 &CamZRange(RndCam *c) { return c->mZRange; }
inline const Hmx::Rect &CamScreenRect(RndCam *c) { return c->mScreenRect; }

// ---- mesh ------------------------------------------------------------------
inline RndMesh *MeshGeomOwner(RndMesh *m) { return m->GeomOwner(); }
// Both shapes hold the platform-compressed vertex blob (an .milo_xbox mesh
// loaded through RB3-Wii's fork keeps the Xbox 36-byte stream verbatim, see
// RndMesh::Load's HX_NATIVE branch); .milo_wii meshes carry uncompressed Verts
// and report 0 here.
inline int MeshNumCompressedVerts(RndMesh *m) {
    return (int)m->GeomOwner()->mNumCompressedVerts;
}
inline unsigned char *MeshCompressedVerts(RndMesh *m) {
    return m->GeomOwner()->mCompressedVerts;
}

inline void VertColor(const RndMesh::Vert &v, float out[4]) {
    out[0] = v.color.fr();
    out[1] = v.color.fg();
    out[2] = v.color.fb();
    out[3] = v.color.fa();
}
inline const Vector2 &VertUV(const RndMesh::Vert &v) { return v.uv; }
inline void VertBoneWeights(const RndMesh::Vert &v, float out[4]) {
    out[0] = v.boneWeights.GetX();
    out[1] = v.boneWeights.GetY();
    out[2] = v.boneWeights.GetZ();
    out[3] = v.boneWeights.GetW();
}

// ---- particles -------------------------------------------------------------
// RB3-Wii particles have no UV-tile animation: one tile, always tile 0.
inline int PartTilesAcross(RndParticleSys *) { return 1; }
inline int PartTilesDown(RndParticleSys *) { return 1; }
inline int PartTileIndex(const RndParticle *) { return 0; }

// ---- cube texture ----------------------------------------------------------
inline RndBitmap *CubeFaceBitmap(RndCubeTex *c, int face) { return &c->mBitmap[face]; }

// ---- draw modes ------------------------------------------------------------
// RB3's WorldReflection::DrawShowing sets mode 7 for the mirrored pass
// (world/Reflection.cpp: SetDrawMode((Mode)7)); DC3 renumbered it to 8 when it
// added kDrawOcclusionDepth and kDrawVelocity.
constexpr int kDrawModeReflection = 7;

// ---- camera select ---------------------------------------------------------
// RB3's shared RndCam::Select sets no viewport. On the Wii the platform camera
// did it after calling the base (rndwii/Cam.cpp WiiCam::Select: GXSetViewport
// with mScreenRect and mZRange), and the native build has no platform camera.
// WgpuRnd therefore applies the same viewport itself when the current camera
// changes (WgpuRnd::ApplyCameraViewport). Without it the depth range stays at
// the zero-initialised [0, 0], every fragment writes depth 0, and under the
// Less test the first opaque draw at each pixel wins -- the sky dome, drawn
// first, hid the whole skyline.
constexpr bool kCamSelectSetsViewport = false;

// ---- render-to-texture -----------------------------------------------------
// RB3's RndTexRenderer::DrawToTexture brackets the draw into its output texture
// with WiiMat::SetOverrideAlphaWrite(true) / (false) (rndobj/TexRenderer.cpp,
// the #ifndef HX_NATIVE block): every material writes destination alpha while
// a render target is bound. The title screen depends on it: clouds_rnd.tex is
// painted by difference_clouds.mat (alpha write off) and composited onto the
// sky by sky_dome.mat with SrcAlpha blending, so without the override the
// target's alpha stays at the clear value 0 and the cloud layer is invisible.
constexpr bool kRenderTargetForcesAlphaWrite = true;

// ---- post-processing -------------------------------------------------------
// RB3's mNoiseIntensity is a gain on a tiled noise TEXTURE (mNoiseMap scaled by
// mNoiseBaseScale), not a per-pixel screen-space add: the menu and venue
// postprocs author it at ~3.0, which as a raw add buries the frame in white
// noise. Convert it to a screen-space grain the way the engine's rb3 flavor
// does (gfx/Shaders/rb3_postproc.wgsl.inc, kNoiseGain): clamp the magnitude to
// 3 and scale by 0.04, a zero-mean swing of about +/-15/255.
inline float PostProcGrain(const RndPostProc *pp) {
    float n = std::fabs(pp->GetNoiseIntensity());
    return (n < 3.0f ? n : 3.0f) * 0.04f;
}

} // namespace rndshape
