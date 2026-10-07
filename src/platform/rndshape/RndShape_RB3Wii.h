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

struct RetailPostParams;   // gfx/RB3RetailPost.h

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

    // (Rnd::EndWorld's world-end step, which also runs the flare point tests,
    // is WgpuRnd::DoWorldEnd on every shape, as retail DxRnd owns it.)
    // Rnd::EndWorld's post step: also grades the world on the GPU
    // (WgpuRnd::FlushWorldPost). Defined in platform/Rnd_Wgpu.cpp.
    void DoPostProcess() override;

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

// Wii WiiMat::Select: a material with use_environ=0 and pre_lit=0 takes its
// colour from the GX register (GX_SRC_REG) -- no ambient, no lights, no vertex
// colour, just material colour x texture. RB3 authors most night-city neon,
// signs, posters, the cloud painter (difference_clouds.mat) and the street
// fog this way. Same rule as the rb3 flavor (RB3MaterialBinder.cpp, mu.unlit).
inline bool MatUnlit(const MatView &m) { return !m.Raw()->mUseEnviron && !m.Raw()->mPreLit; }
// RB3 retail (xbox_shaders `standard`, prelit bit 8): a prelit material that
// uses the environ is lit as vertexColour * ambient + approx + point lights,
// all times the material colour; only use_environ=0 keeps the bare vertex
// colour (NgMat::SetupAmbient loads ambient 1). Only the retail light model
// (SceneUniforms.retailLighting) reads this.
inline bool MatPrelitAmbient(const MatView &m) { return m.Raw()->mPreLit && m.Raw()->mUseEnviron; }

// RB3's Xbox 360 material terms (RndShape.h). The Wii RndMat::Load reads them
// and drops them; with RB3_NATIVE_XBOX_MAT_FIELDS the consumer's RndMat keeps
// them (mXb* members). The selection is retail
// RndShaderStandard::CalcShaderOpts with TheShaderMgr.AllowPerPixel() true
// (its default); the constants are NgMat's: c2 = (specular rgb,
// max(power, 0.5)), c63 = (rim rgb, max(power, 0.5)), c14.x = 1 - deNormal.
// Whether lights exist at all (CalcShaderOpts gates every term on real or
// approx lights) is a per-draw test the shader makes.
inline bool PackNonZero(const Hmx::Color &c) {
    // Hmx::Color::Pack(): rgb as truncated bytes, alpha ignored.
    return (((int)(c.red * 255.0f) & 0xFF) | (((int)(c.green * 255.0f) & 0xFF) << 8)
            | (((int)(c.blue * 255.0f) & 0xFF) << 16)) != 0;
}
// RB3 RndMat colour modulation: mColorModFlags (0 none, 1 AlphaPack,
// 2 AlphaUnpackModulate, 3 Modulate) and the three mColorMod colours the band
// modes scale by. Crowd::Init gives each 3D crowd character's materials
// Modulate with three ColorPalette picks (world/Crowd.cpp); retail
// NgMat::SetupShader loads the colours into c131..c133 whenever the flags are
// set. Returns the mode and fills colours[i] = rgb, 1; a band mode with fewer
// than three colours reads as none.
inline int MatColorMod(const MatView &m, float colours[3][4]) {
    RndMat *mat = m.Raw();
    const int mode = (int)mat->mColorModFlags;
    if (mode == RndMat::kColorModAlphaPack) return mode;
    if (mode != RndMat::kColorModAlphaUnpackModulate && mode != RndMat::kColorModModulate)
        return 0;
    if (mat->mColorMod.size() < 3) return 0;
    for (int i = 0; i < 3; i++) {
        const Hmx::Color &c = mat->mColorMod[i];
        colours[i][0] = c.red;
        colours[i][1] = c.green;
        colours[i][2] = c.blue;
        colours[i][3] = 1.0f;
    }
    return mode;
}

// World refraction (RndShape.h RefractTerms) as the Xbox RndMat answers it:
// GetRefractEnabled is mRefractEnabled && mRefractStrength > 0 && a normal
// map, and GetRefractNormalMap is mRefractNormalMap, else the material's
// normal map (rb3-xenon rndobj/Mat.cpp). The Wii class has no normal map
// besides the Xbox one carried for the native build.
inline bool MatRefract(const MatView &m, RefractTerms &r) {
    RndMat *mat = m.Raw();
    RndTex *normal = mat->mRefractNormalMap;
#ifdef RB3_NATIVE_XBOX_MAT_FIELDS
    if (!normal) normal = mat->mXbNormalMap;
#endif
    if (!mat->mRefractEnabled || !(mat->mRefractStrength > 0.0f) || !normal) return false;
    r.strength = mat->mRefractStrength;
    r.normalTex = normal;
    return true;
}

inline bool MatRetailTerms(const MatView &m, RetailMatTerms &t) {
#ifdef RB3_NATIVE_XBOX_MAT_FIELDS
    RndMat *mat = m.Raw();
    if (!mat->mUseEnviron) return false;
    t = RetailMatTerms();
    const Hmx::Color &spec = mat->mXbSpecularRGB;
    t.specular = PackNonZero(spec);
    t.specular_rgb[0] = spec.red; t.specular_rgb[1] = spec.green; t.specular_rgb[2] = spec.blue;
    t.specularPower = spec.alpha > 0.5f ? spec.alpha : 0.5f;
    if (mat->mXbPerPixelLit) {
        t.perPixel = true;
        t.normalTex = mat->mXbNormalMap;
        t.normalMap = t.normalTex != nullptr;
        t.specularTex = t.specular ? (RndTex *)mat->mXbSpecularMap : nullptr;
        t.specularMap = t.specularTex != nullptr;
        const Hmx::Color &rim = mat->mXbRimRGB;
        t.rim = PackNonZero(rim);
        t.rim_rgb[0] = rim.red; t.rim_rgb[1] = rim.green; t.rim_rgb[2] = rim.blue;
        t.rimPower = rim.alpha > 0.5f ? rim.alpha : 0.5f;
        t.rimLightUnder = t.rim && mat->mXbRimLightUnder;
        t.rimTex = t.rim ? (RndTex *)mat->mXbRimMap : nullptr;
        t.rimMap = t.rimTex != nullptr;
        t.deNormal = mat->mXbDeNormal;
    }
    return true;
#else
    (void)m; (void)t;
    return false;
#endif
}

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
// A relative (non-world-space) RB3 system stores particles in the system's
// frame; world position = mRelativeXfm * p->pos (Part.h). Absolute systems keep
// mRelativeXfm at identity.
inline Vector3 PartWorldPos(RndParticleSys *s, RndParticle *p) {
    Vector3 w;
    Multiply(p->Pos3(), s->RelativeXfm(), w);
    return w;
}
// The Wii TEV multiplies the particle colour by the material register colour,
// and RB3's translucent haze systems (material alpha < 1: street fog, the cloud
// painter's wisps) are tuned for that dimmer GX blend. Same model as the rb3
// flavor's BandRnd::DrawParticles: tint by material colour, scale haze alpha
// by 0.35, and fade haze out within two half-sizes of the camera.
constexpr bool kPartMaterialTint = true;

// ---- cube texture ----------------------------------------------------------
inline RndBitmap *CubeFaceBitmap(RndCubeTex *c, int face) { return &c->mBitmap[face]; }

// ---- texture layout --------------------------------------------------------
// RB3 Wii bitmaps (.milo_wii inline textures and .png_wii files) carry GX pixel
// layouts, marked by the 0x40 order bit (RndTex::PlatformBppOrder on Wii):
// CMPR, two-plane CMPR colour + alpha, RGBA8 and I8 tiles. The Wii decoded them
// in hardware; TextureConvert decodes them to RGBA8 (gfx/GxTextureDecode).
constexpr bool kGxTextureLayout = true;

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
// The standard shader sRGB-encodes its output (gfx/standard_wgsl.inc,
// linearToSrgb) for the non-sRGB surface, and a render target (RGBA8Unorm on
// that surface) is sampled back as raw data, so a material drawn through a
// target would be encoded twice. Draws into a target therefore skip the
// encode. Under kGammaSpaceShading (below) no RB3 draw encodes at all, in a
// target or in the frame, so this only matters to a draw that is not
// gamma-shaded; the cloud target (clouds_rnd.tex) holds the same values as on
// retail either way.
constexpr bool kRenderTargetStoresLinear = true;

// ---- 2D rects --------------------------------------------------------------
// RB3's DrawRect colour is the material's register colour times the colour
// argument. OutfitConfig::MatSwap::Compose relies on it: every rect it draws
// into an outfit's *_output target passes white and carries the palette tint
// in sMat->SetColor(), so without the material colour every recoloured
// garment, hair and eye composes to its untinted grey detail map.
constexpr bool kRectModulatesMatColor = true;

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
// RB3 grades through its retail Xbox 360 post chain (gfx/RB3RetailPost: the
// pseudo-HDR luminance bloom mask, three quarter-size blurred bloom sets
// screen-blended at bloomColor * BloomIntensity(), then the RndColorXfm matrix)
// instead of PostProcPass's DC3 composite, which clamps RB3's bloom to almost
// nothing (intensity <= 1, threshold >= 0.7, a quarter-strength screen) and
// grades from contrast/brightness/saturation/levels alone, dropping hue and
// lightness (the title's drop_fade.pp fades in through lightness -100).
// FillRetailPost is defined in rndshape/RB3WiiPostChain.cpp; `flickerMul` is
// PostProcPass's flicker modulation, applied as NgPostProc::ModulateColorXfm
// applies mColorModulation (to the 3x3 part only).
constexpr bool kRetailPostChain = true;
void FillRetailPost(const RndPostProc *pp, float flickerMul, ::RetailPostParams &out);
// MILO_RB3_RETAIL_POST: unset or anything else = 1 (retail chain), "0" = 0
// (PostProcPass's DC3 composite, as before the chain existed), "raw" = 2
// (scene passed through ungraded), "mask" = 3 / "bloom" = 4 (the retail chain,
// showing the bloom mask or the bloom term instead; for inspection), "grade" =
// 5 (the chain without bloom), "beams" = 6 (the chain, showing the blurred
// spotlight beam target instead; gfx/SpotBeamPass).
int RetailPostMode();
// The frame's alpha carries the bloom mask this frame (the retail chain, or
// its mask/bloom inspection views).
inline bool RetailBloomMaskActive() {
    const int m = RetailPostMode();
    return m == 1 || m == 3 || m == 4 || m == 6;
}
// The pseudo-HDR bloom mask (ShaderOptions bit 22): retail's standard.ps writes
// a = dot(rgb, c7.rgb) for a material whose NgMat::AllowHDR() holds, drawn into
// the main frame (CalcShaderOpts: !fadeOut && !offscreen && AllowHDR()); every
// other material leaves the destination alpha alone, and the frame clears it to
// 0 (DxRnd::BeginDrawing packs only the clear colour's RGB). bloom.ps then
// weights the scene by that alpha. Returns c7's scale (SetBloomColor:
// 1/threshold above 1, else 1) for such a material while the retail chain runs
// with a current RndPostProc, else 0 (no mask). The caller decides "main frame".
float BloomMaskScale(const MatView &m);
// RB3 shades in gamma space: retail's standard.ps multiplies the texel by the
// material colour and the ambient + diffuse lighting sum as stored and
// authored, and writes the product straight to an 8-bit target
// (D3DFMT_A8R8G8B8, DxRnd::CreateEDRAMSurfaces); render targets are the same
// format and are sampled back as stored. The bitmap formats
// (DxRnd::D3DFormatForBitmap: D3DFMT_DXT1/3/5, A8R8G8B8, ...) carry no gamma
// sign, so the texture fetch does not linearize either. The dc3 shader works
// in linear light and encodes its output; under this flag it skips both the
// decodes and the encode and computes retail's t * c * L directly
// (standard_wgsl.inc, material.gammaShading; particles likewise, Part_Wgpu).
// Display gamma is separate: see DisplayGamma below.
constexpr bool kGammaSpaceShading = true;
// The display gamma ramp. DxRnd::InitRenderState ends in DxRnd::SetupGamma,
// which reads the system config's (rnd (gamma g)) and, when present, sets
// D3DDevice_SetGammaRamp with entry i = (u16)(pow(i / 256, g) * 1024) * 64
// for all three channels. Every shipped default.dtb (Xbox and Wii) carries
// (gamma 0.85), so the Xbox 360 scans the frame out through x^0.85 and the
// frame buffer itself stays darker than what is seen. The Wii renderer has no
// equivalent: no GXSetDispCopyGamma or VI gamma call outside the SDK, so a Wii
// frame is displayed as rendered. The ramp sits between the front buffer and
// the screen, so it applies only to a frame that is `presenting` (a window
// surface or the web canvas). A headless frame is the front buffer, which is
// what a retail screenshot is: xenia's raw frame dump of the retail XEX is
// also pre-ramp and matches the TCRF title screenshot better than the ramped
// one does (dc3-backend-for-rb3-wii.md, section 10). Returns g, or 0 for no
// ramp. MILO_RB3_DISPLAY_GAMMA overrides it for every output, headless
// included ("off" or "0" = no ramp, else the value). Defined in
// rndshape/RB3WiiPostChain.cpp; applied by WgpuRnd::EndDrawing
// (gfx/DisplayRamp) to the whole frame, after everything else is drawn.
float DisplayGamma(bool presenting);

// ---- scene lighting --------------------------------------------------------
// RB3 lights with its own (Xbox 360 retail) model, not DC3's venue rig:
// world.cam takes the environ's ambient and up to two real point lights with
// retail's linear falloff (per environ, here), plus the approx lights folded
// into a box map per mesh (FillMeshApproxLighting); every other camera gets a
// flat key. Defined in rndshape/RB3WiiSceneLighting.cpp (built only for this
// shape under the dc3 backend). Returns true, so WgpuRnd skips its DC3
// lighting block.
bool WriteSceneLighting(SceneUniforms &s, RndCam *cam);
// The approx-light box map for one mesh draw (ObjectUniforms.boxLight, six
// faces {+X,-X,+Y,-Y,+Z,-Z}). Retail recomputes it per mesh at the mesh's
// world sphere centre (RndMesh::sUpdateApproxLight), and once per character
// at the character's sphere centre (Character::DrawLodOrShadow). Leaves the
// faces zero when the retail light model does not apply to this draw.
// retail[0] = 1 when the mesh carries baked ambient occlusion
// (RndMesh::HasAOCalc, retail's TheShaderMgr.UseAO()), retail[1] = the number
// of approx lights queued (NgEnviron::UpdateApproxLighting); both 0 when the
// retail light model does not apply.
void FillMeshApproxLighting(RndMesh *mesh, float box[6][4], float retail[4]);

// ---- per-draw state log ----------------------------------------------------
// The draw log and provenance sidecar RB3's harnesses read (RB3DrawLogDebug.h:
// RB3_DRAWLOG, RB3_DRAWLOG_DUMP, RB3_DRAWLOG_PROV, RB3DebugSetDrawLogEnabled).
// Defined in rndshape/RB3WiiDrawLog.cpp. DrawLogActive is a cached env test;
// the mesh path builds a DrawLogDraw only when it is true.
bool DrawLogActive();
// Clear the frame's log (WgpuRnd::BeginDrawing).
void DrawLogFrameBegin();
// Write RB3_DRAWLOG_DUMP if set (WgpuRnd::EndDrawing).
void DrawLogFrameEnd(int frame);
// A mesh render pass opened; depthOp 0 Clear, 1 Load, 2 no depth attachment.
void DrawLogPassOpen(int depthOp);
// Record one mesh draw (and its provenance entry when RB3_DRAWLOG_PROV is set).
void DrawLogRecord(const DrawLogDraw &d);

} // namespace rndshape
