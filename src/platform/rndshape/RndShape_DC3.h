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

// The camera planes DC3's NativeDOFProc kept at Set (dc3-decomp
// src/system/rndobj/DOFProc.cpp defines it). rb3-xenon also compiles this
// shape and has no such function, so the engine carries a weak default that
// answers false (DC3PostChain.cpp): rb3-xenon's depth of field keeps reading
// the current camera, as before.
bool NativeDOFSetCamPlanes(float &nearPlane, float &farPlane, float &zNear, float &zFar);

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

// rb3-xenon: RB3's own Xbox 360 light model, the engine's retail form 1
// (SceneUniforms.retailLighting 1, docs/native/dc3-backend-for-rb3-wii.md
// section 9; the shader branch the RB3-Wii shape also drives). Retail RB3
// X360 standard shading is c0 * (c1 ambient + box(N) + at most two point
// lights) * texel, the box map c80..c85 written per mesh by
// NgEnviron::UpdateApproxLighting (rb3-xenon rndobj/Env_NG.cpp). Before
// lane A62 rb3-xenon lit through WgpuRnd::WriteSceneUniforms' heuristic arm
// (approx lights as top-4 directionals by name, a 0.08 ambient floor, a 0.35
// fill, an energy cap, soft clip), none of which retail has.
//
// Detected structurally: not DC3, and an environ that runs RB3's own approx
// lighting -- the virtual UpdateApproxLighting(const Vector3 *) that
// NgEnviron overrides, plus the local/global approx switches it reads.
// RndEnviron is complete here (kDC3RetailModelFor's is_base_of needs that),
// so a false answer is a real absence, not an incomplete type; the rb3-app
// gate rb3_app_check_retail_light_model() requires it true for rb3-xenon.
template <class E, class = void> struct HasRB3ApproxLighting : std::false_type {};
template <class E>
struct HasRB3ApproxLighting<
    E, std::void_t<decltype(std::declval<E &>().UpdateApproxLighting(
                       static_cast<const Vector3 *>(nullptr))),
                   decltype(std::declval<const E &>().UsesApproxLocal()),
                   decltype(std::declval<const E &>().UsesApproxGlobal()),
                   decltype(std::declval<const E &>().NumLights_Approx())> >
    : std::true_type {};
template <class T>
constexpr bool kRB3RetailLightFor =
    !kDC3RetailModelFor<T> && HasRB3ApproxLighting<typename DependentEnvT<T>::type>::value;
constexpr bool kRB3RetailLight = kRB3RetailLightFor<void>;

// Whether a material ignores lighting entirely (register colour x texture, no
// ambient, lights or vertex colour). DC3 (retail model): a material with
// use_environ off that is not prelit. RndShaderStandard::CalcShaderOpts gates
// both light bits (16 real, 17 approx) on mat->UseEnviron(), and
// NgMat::SetupAmbient puts (1,1,1,1) in c1 for such a material, so the shipped
// permutation 0x10 draws texel * c0 * 1 (100% matched; shaders section of the
// doc). rb3-xenon: no such state.
template <class T>
inline bool MatUnlit(const T &m) {
    // rb3-xenon (kRB3RetailLightFor): the same rule, RB3's own -- its
    // RndShaderStandard::CalcShaderOpts gates both light bits on
    // mat->UseEnviron() (rndobj/Shader.cpp) and NgMat::SetupAmbient loads
    // (1,1,1,1) into c1 when use_environ is off (rndobj/Mat_NG.cpp).
    if constexpr (kDC3RetailModelFor<T> || kRB3RetailLightFor<T>)
        return m && !m->UseEnviron() && !m->Prelit();
    else { (void)m; return false; }
}
// Whether a prelit material's vertex colour is its ambient term (lights add
// on top). DC3 (retail model): prelit and use_environ, as permutation 0x20110
// computes (vertex * c1 + box) * c0 with c1 the environ's ambient. Prelit
// without use_environ (0x100 with c1 = 1) is the vertex colour as authored.
template <class T>
inline bool MatPrelitAmbient(const T &m) {
    // rb3-xenon: RB3 standard.vs prelit bit 8, as the RB3-Wii shape reads it.
    if constexpr (kDC3RetailModelFor<T> || kRB3RetailLightFor<T>)
        return m && m->Prelit() && m->UseEnviron();
    else { (void)m; return false; }
}
// Whether a material's ambient constant c1 is the current environ's ambient,
// alpha included (gfx/AmbientAlpha.h). rb3-xenon: NgMat::SetupAmbient
// (band.exe TU5 fn_824A9090) +0x18 tests use_environ and loads the environ's
// ambient into c1, else (1, 1, 1, 1). DC3 the same: NgMat::SetupAmbient
// (373307D9 fn_8269CCA8, dc3-decomp rndobj/Mat_NG.cpp) loads
// RndEnviron::Current()->AmbientColor(), w included, into kVS_AmbientColor and
// kPS_AmbientColor when use_environ is set, else (1, 1, 1, 1); and DC3's
// standard programs carry c1.w into oC0.w (standard_100010030011 PS
// `maxs r0._, c0.ww` ... `muls_prev r0.___w, c1.w`; `mul oC0, r0, r1`,
// docs/native/dc3-retail-lighting.md "ambient alpha"). DC3's UI fades and
// hides groups this way: cursor.milo depth_buffer_{left,right}.env, mic.env,
// meter.env, invoke_indicator.env and movie_pause's fade envs are authored at
// ambient alpha 0 and keyed by PropAnims (lane A77's milo scan).
template <class T>
inline bool MatAmbientIsEnviron(const T &m) {
    if constexpr (kRB3RetailLightFor<T> || kDC3RetailModelFor<T>)
        return m && m->UseEnviron();
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
// rb3-xenon (kRB3RetailLightFor) takes the same terms from RB3's own
// CalcShaderOpts (rb3-xenon rndobj/Shader.cpp, RndShaderStandard at band.exe
// TU5 fn_824A66E8): specular on SpecularRGB().Pack(), per-pixel on
// AllowPerPixel && GetPerPixelLit, then normal map, specular map (only with
// specular), rim on RimRGB().Pack() (not gated on specular), rim-under and
// rim map; it has no flip_normal ("Retail RB3 X360 has no mFlipNormal here").
template <class T>
inline bool MatRetailTerms(const T &m, RetailMatTerms &t) {
    if constexpr (kDC3RetailModelFor<T> || kRB3RetailLightFor<T>) {
        if (!m || !m->UseEnviron()) return false;
        // rb3-xenon skin keeps the native skin arm (the shipped skin
        // permutation, standard_wgsl.inc rb3SkinDiffuseOn; kSkinSpecRB3Xenon),
        // which reads the material's own fields and, under the retail light
        // model, the real box map. The generic retail terms would zero them.
        if constexpr (kRB3RetailLightFor<T>) {
            if (m->GetShaderVariation() == kShaderVariationSkin) return false;
        }
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
            if constexpr (kDC3RetailModelFor<T>) t.flipNormal = m->GetCull() == kCullBackwards;
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
// Screen-aligned texture coordinates (np-gfx-light round 5): ShaderOptions bit
// 13, which RndShaderStandard::CalcShaderOpts sets from
// BaseMaterial::GetScreenAligned(). The shipped permutation with it
// (standard_0000000000002010.vs, xenia-gpu-shader-compiler ucode) drops the
// vertex UV and puts the vertex's own screen position through the texgen rows
// instead:
//   r0.xy = oPos.xy * rcp(oPos.w) * 0.5 + 0.5;  r0.z = 1 - r0.y
//   o0.x = dp2add(r0.zx, c20.yx, c20.w);  o0.y = dp2add(r0.zx, c21.yx, c21.w)
// where the plain 0x10 permutation feeds the vertex UV to the same two
// dp2adds. Streetside's peak sky carries the flag (sky_peak_1.mat and
// sky_peak_2.mat read back screen_aligned 1 on native; peak_street.mat 0);
// with vertex UVs the sky cylinder showed its starfield where
// retail shows the searchlight fan, and the mirror floor reflected stars where
// retail reflects the texture's navy bottom. rb3-xenon keeps vertex UVs.
template <class T>
inline bool MatScreenAligned(const T &m) {
    if constexpr (kDC3RetailModelFor<T>) return m && m->GetScreenAligned();
    else { (void)m; return false; }
}

// The texture-transform rows the DC3 image hands the standard vertex shader
// for kTexGenXfm and kTexGenXfmOrigin (docs/native/dc3-retail-lighting.md
// section 11). NgMat::SetupShader builds mTexGenMatrix with MakeTex3
// (dc3-decomp src/system/rndobj/Mat_NG.cpp): rows x = (m.x.x, -m.x.y),
// y = (-m.y.x, m.y.y), and for the centred mode a translation that is applied
// before the matrix with tx negated, w = M * (-tx - 0.5, ty - 0.5) + 0.5; the
// about-origin mode uses w = (tx, ty). SetVConstant(kVS_TexTransform) uploads
// it transposed and the shader (standard_0000000000000010.vs, and the
// screen-aligned 0x2010) evaluates u' = dp2add(uv.yx, c20.yx, c20.w), i.e.
//   u' = m.x.x*u - m.y.x*v + w.x,   v' = -m.x.y*u + m.y.y*v + w.y.
// Written here in the engine shader's encoding: the kTexGenXfm branch reads
// dot(uv - 0.5, row.xy) + row.z + 0.5, the kTexGenXfmOrigin branch
// dot(uv, row.xy) + row.z. Returns false (and leaves the rows alone) for any
// other mode and for rb3-xenon, which keep the generic rows.
template <class T>
inline bool MatRetailTexXfmRows(const T &m, float row0[4], float row1[4]) {
    if constexpr (kDC3RetailModelFor<T>) {
        if (!m) return false;
        const TexGen tg = m->GetTexGen();
        if (tg != kTexGenXfm && tg != kTexGenXfmOrigin) return false;
        const Transform &x = m->TexXfm();
        const float ux = x.m.x.x, uy = -x.m.y.x;   // u' = ux*u + uy*v + uw
        const float vx = -x.m.x.y, vy = x.m.y.y;   // v' = vx*u + vy*v + vw
        if (tg == kTexGenXfm) {
            const float ntx = -x.v.x - 0.5f, ty = x.v.y - 0.5f;
            const float uw = ux * ntx + ty * uy + 0.5f;
            const float vw = vx * ntx + ty * vy + 0.5f;
            row0[0] = ux; row0[1] = uy; row0[2] = uw + 0.5f * (ux + uy) - 0.5f;
            row1[0] = vx; row1[1] = vy; row1[2] = vw + 0.5f * (vx + vy) - 0.5f;
        } else {
            row0[0] = ux; row0[1] = uy; row0[2] = x.v.x;
            row1[0] = vx; row1[1] = vy; row1[2] = x.v.y;
        }
        row0[3] = 0.0f;
        row1[3] = 0.0f;
        return true;
    } else {
        (void)m; (void)row0; (void)row1;
        return false;
    }
}

// ---- environment -----------------------------------------------------------
inline RndEnviron *CurrentEnv() { return RndEnviron::Current(); }
inline bool EnvHasAmbientFogOwner(RndEnviron *e) { return e->AmbientFogOwner() != nullptr; }
inline float EnvFogStart(RndEnviron *e) { return e->FogStart(); }
inline float EnvFogEnd(RndEnviron *e) { return e->FogEnd(); }
inline ObjPtrList<RndLight> &EnvLightsApprox(RndEnviron *e) { return e->LightsApprox(); }
inline ObjPtrList<RndLight> &EnvLightsReal(RndEnviron *e) { return e->LightsReal(); }

// The environ's left/right fade, detected: only a game whose RndEnviron
// exposes LRFade() (rb3-xenon) takes it; dc3-decomp's does not, so its scene
// block keeps zero planes and its frames do not change. The planes are
// NgEnviron::Select's (band.exe TU5 fn_82B86738; rb3-xenon rndobj/Env_NG.cpp;
// NgEnviron::Select uploads them as VConstant/PConstant 0x35 and 0x36), built
// only when the environ fades out (mFadeOut and FadeEnd != FadeStart).
// The depth half of that fade (constant 0x37: end, 1 / (end - start), max)
// is not implemented.
template <class E, class = void> struct EnvHasLRFade : std::false_type {};
template <class E>
struct EnvHasLRFade<E, std::void_t<decltype(std::declval<const E &>().LRFade()),
                                   decltype(std::declval<const E &>().LRFadeRef())>>
    : std::true_type {};
template <class E>
inline bool EnvLRFadePlanes(E *e, float left[4], float right[4]) {
    if constexpr (!EnvHasLRFade<E>::value) {
        (void)e; (void)left; (void)right;
        return false;
    } else {
        if (!e || !e->FadeOut() || e->FadeEnd() == e->FadeStart()) return false;
        const auto &lr = e->LRFade();
        const Transform ref = e->LRFadeRef();
        Vector3 dir = ref.m.x;
        Normalize(dir, dir);
        const float refDot = Dot(ref.v, dir);
        left[0] = left[1] = left[2] = 0.0f; left[3] = 1.0f;
        right[0] = right[1] = right[2] = 0.0f; right[3] = 1.0f;
        if (lr.x != lr.y) {
            const float s = 1.0f / (lr.y - lr.x);
            left[0] = dir.x * s; left[1] = dir.y * s; left[2] = dir.z * s;
            left[3] = -((lr.x + refDot) * s);
        }
        if (lr.z != lr.w) {
            const float s = 1.0f / (lr.z - lr.w);
            right[0] = dir.x * s; right[1] = dir.y * s; right[2] = dir.z * s;
            right[3] = -((lr.w + refDot) * s);
        }
        return true;
    }
}
// A material that declares fade_out (RndMat mFadeout), detected the same way.
template <class T, class = void> struct MatHasFadeOut : std::false_type {};
template <class T>
struct MatHasFadeOut<T, std::void_t<decltype(std::declval<const T &>()->FadeOut())>>
    : std::true_type {};
template <class T>
inline bool MatFadesOut(const T &m) {
    if constexpr (MatHasFadeOut<T>::value && EnvHasLRFade<RndEnviron>::value) return m && m->FadeOut();
    else { (void)m; return false; }
}

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
// The planes the depth of field's constants come from (DofPass::Run). DC3's
// NgDOFProc::Set computes its scale and bias from the camera it is given, at
// Set (DOFProc_NG.cpp); dc3's NativeDOFProc keeps that camera's planes
// (rndobj/DOFProc.cpp). False when Set had no camera: the caller reads the
// current camera, as before.
inline bool DofSetCamPlanes(float &nearPlane, float &farPlane, Vector2 &zRange) {
    return NativeDOFSetCamPlanes(nearPlane, farPlane, zRange.x, zRange.y);
}

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
// Whether the system's quads read one tile of a tile sheet (animate_uvs) or the
// whole texture. The shipped particles vertex shader (dc3 xbox_shaders, type
// "particles", every textured permutation) branches on c51.x, which
// DxParticleSys::DrawShowing loads with mAnimateUVs ? 1 : 0 (target 0x82625690,
// lbz of the flag, fmr 1.0 or 0.0, SetVConstant 0x33):
//   (p0 = c51.x == 0)  max r3.xy, r0.yx                  the corner's own 0/1 uv
//   (!p0)              r3.xy = (corner + (col, row)) * c50.zw   one tile
// so with animate_uvs off num_tiles_across/down are ignored and each quad
// carries the whole texture (throneroom's smoke.mat systems: a 6 x 5 sheet
// drawn whole, 30 small puffs per quad -- the original's speckled haze).
// A consumer whose RndParticleSys has no AnimateUVs() accessor keeps reading
// the tile at mCurrentTileIndex for every system, as before (rb3-xenon, whose
// retail DrawShowing loads neither c50 nor c51).
template <typename Sys, typename = void>
struct PartHasAnimateUVs : std::false_type {};
template <typename Sys>
struct PartHasAnimateUVs<Sys, std::void_t<decltype(std::declval<const Sys &>().AnimateUVs())>>
    : std::true_type {};
template <typename Sys>
inline bool PartAnimatesUVs(Sys *s) {
    if constexpr (PartHasAnimateUVs<Sys>::value) {
        return s->AnimateUVs();
    } else {
        (void)s;
        return true;
    }
}
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
// The billboard of the shipped particles vertex shader (dc3 xbox_shaders,
// type "particles", every permutation; its non-velocity-aligned path, read
// from the microcode). DxParticleSys::DrawShowing passes c47 = 0.5 * camera X
// and c48 = 0.5 * mScreenAspect * camera Z (both in the system's relative
// space), and each vertex carries size (t0), angle (t1) and swingArm (t2).
// For corner signs p (right) and q (up) the shader places
//
//   pos + size/2 * (p * (cos a R - sin a S U) + q * (sin a R + cos a S U))
//       + swingArm * (sin a R + cos a S U)
//
// with R/U the camera right/up axes and S the screen aspect. So:
//   - screen_aspect scales the quad's height (streetside alleySteam: 0.7);
//   - the swing arm moves the sprite along its own rotated up axis, so a
//     spinning particle with a start/end_offset circles its position
//     (throneroom's smoke systems: spin with start_offset 10);
//   - a growing angle turns the quad clockwise on screen (the engine's old
//     billboard turned it anticlockwise).
// A consumer whose RndParticleSys has no ScreenAspect() accessor keeps the old
// billboard (rb3-xenon, whose own retail DxParticleSys sets the same
// constants, until it adds the accessor).
//
// Velocity-aligned systems (velocity_align) take the shader's other path.
// DxParticleSys::DrawParticles then writes 2 * vel into the slots that carry
// angle and swing arm, keyed on mAlignWithVelocity alone; DrawShowing passes
// c49 = (aligned, stretched, constantArea, 2 * stretch_scale), with
// aligned = fancy && velocity_align, stretched = aligned && stretch_with_velocity,
// constantArea = stretched && constant_area, and c47.w = 0 only when
// fancy && aligned && stretched && perspective. ParticleAlignedAxes
// (Part_Wgpu.cpp) carries the decode. One retail consequence is kept:
// velocity_align on a basic (non-fancy) pool sends the velocity down the
// billboard path, read as angle = 2 * vel.x and swing arm = 2 * vel.y. No
// shipped DC3 system does that (round 3 census: every aligned system is fancy).
// A consumer whose RndParticleSys has no AlignWithVelocity() accessor keeps the
// billboard for every system, as before.
struct PartBillboardTerms {
    bool retailXbox = false;
    float screenAspect = 1.0f;
    bool velocityVerts = false;  // mAlignWithVelocity: the vertex carries 2 * vel
    bool aligned = false;        // c49.x
    bool stretched = false;      // c49.y
    bool constantArea = false;   // c49.z
    bool perspective = false;    // !c47.w
    float stretchScale = 1.0f;   // c49.w / 2
    // Rows of mRelativeXfm.m: the space p->pos and p->vel live in.
    float rel[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};
template <typename Sys, typename = void>
struct PartHasScreenAspect : std::false_type {};
template <typename Sys>
struct PartHasScreenAspect<Sys, std::void_t<decltype(std::declval<const Sys &>().ScreenAspect())>>
    : std::true_type {};
template <typename Sys, typename = void>
struct PartHasVelocityAlign : std::false_type {};
template <typename Sys>
struct PartHasVelocityAlign<
    Sys, std::void_t<decltype(std::declval<const Sys &>().AlignWithVelocity())>>
    : std::true_type {};
template <typename Sys>
inline PartBillboardTerms PartBillboard(Sys *s) {
    PartBillboardTerms t;
    if constexpr (PartHasScreenAspect<Sys>::value) {
        t.retailXbox = true;
        t.screenAspect = s->ScreenAspect();
        if constexpr (PartHasVelocityAlign<Sys>::value && PartHasRelativeXfm<Sys>::value) {
            const bool fancy = s->IsFancy();
            t.velocityVerts = s->AlignWithVelocity();
            t.aligned = fancy && t.velocityVerts;
            t.stretched = t.aligned && s->StretchWithVelocity();
            t.constantArea = t.stretched && s->ConstantArea();
            t.perspective = t.stretched && s->PerspectiveStretch();
            t.stretchScale = s->StretchScale();
            const auto &m = s->RelativeXfm().m;
            const float rows[9] = {m.x.x, m.x.y, m.x.z, m.y.x, m.y.y,
                                   m.y.z, m.z.x, m.z.y, m.z.z};
            for (int i = 0; i < 9; i++) t.rel[i] = rows[i];
        }
    } else {
        (void)s;
    }
    return t;
}
inline float PartSwingArm(const RndParticle *p) { return p->swingArm; }
// The particle's velocity, in the same space as p->pos.
inline Vector3 PartVelocity(const RndParticle *p) {
    return Vector3(p->vel.x, p->vel.y, p->vel.z);
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

// ---- bitmap mip levels -----------------------------------------------------
// How many levels of a RndBitmap texture reach the GPU, base included
// (0 = every authored mip). DC3's DxTex::SyncBitmap (rnddx9/Tex.cpp) clamps
// the level count it passes to CreateTexture/CreateSurface to 2:
//     82614738  bl     RndBitmap::NumMips
//     8261473C  addi   r24, r3, 0x1      ; numLevels = NumMips() + 1
//     82614740  cmpwi  cr6, r24, 0x2
//     82614744  ble    cr6, +8
//     82614748  li     r24, 0x2          ; if (numLevels > 2) numLevels = 2
// so a minified retail texture blends between its base and first mip only,
// never the blurrier mips below. Cross-checked on the running original under
// Xenia: the live fetch constants (TheDxRnd.mD3DDevice + 0x480) of dci's
// world, character and HUD samplers read mip_max_level <= 1 throughout a song.
// Uploading the whole authored chain made every minified prop and the
// cylindrical HUD read ~0.65-0.8 of the original's sharpness.
// rb3-xenon's Tex.cpp SyncBitmap is not decompiled, so it keeps the full chain.
constexpr int kBitmapMaxMipLevels = kDC3RetailModel ? 2 : 0;

// ---- alpha cut -------------------------------------------------------------
// DC3's alpha cut is the alpha test and nothing else: NgMat::SetBasicState
// (dc3 src/system/rndobj/Mat_NG.cpp) sets
//     SetAlphaTestEnable(mAlphaCut);
//     if (mAlphaCut) SetAlphaFunc(5, mAlphaThreshold);
// and RndRenderState::SetAlphaFunc (0x82625D40) maps TestFunc 5 through its
// table at 0x8208A138 to 4, the 360's D3DCMP_GREATER: a pixel passes when its
// alpha is ABOVE the threshold, so a cut at 0 drops alpha-0 texels.
// Nothing in the image calls D3DDevice_SetRenderState_AlphaToMaskEnable
// (0x82BA2D60; its only references are the d3d9i state tables), so coverage
// never follows alpha.
//
// kAlphaCutUsesCoverage: the cut also turns on hardware alpha-to-coverage
// (coverage = output alpha) in a multisampled pass. rb3-xenon (also this shape)
// keeps it: its NgMat is not decompiled. DC3 never does: with coverage on, a
// texel of alpha a covers a of the samples, so a premultiplied glyph drawn at
// alpha a resolved to a * a of its colour -- song select's list rows (the
// Eagle-Light(47) font: alpha_cut 1, threshold 0, PreMultAlpha, faded to 0.5)
// drew at ~0.6 of the original and every glyph lost its soft edge.
// DC3's enhanced look (DC3_LOOK, platform/LookProfile.h) instead anti-aliases
// the edge of an otherwise opaque cutout with a sharpened coverage mask
// (MaterialCutCoverage, platform/MaterialSetup.h).
constexpr bool kAlphaCutUsesCoverage = !kDC3RetailModel;
// Whether the cut is retail's strict "alpha > threshold" (DC3) rather than the
// native port's "alpha >= threshold" (rb3-xenon, unchanged).
constexpr bool kAlphaTestGreater = kDC3RetailModel;

// ---- draw modes ------------------------------------------------------------
// The Rnd::Mode WorldReflection::DrawShowing sets while it draws the mirrored
// world. The two consumers of this shape number it differently: DC3 stores 8
// (dc3 world/Reflection.cpp: SetDrawMode((Rnd::Mode)8)), and RB3 retail stores
// 7 (band.exe WorldReflection::DrawShowing, `li r10,7` then `stw r10,0xfc(r11)`
// -- Rnd +0xfc is the draw mode -- at 0x824E8994; rb3-xenon world/Reflection.cpp
// matches it). This used to be 8 for both, so on
// rb3-xenon no draw ever knew it was in the mirrored pass and the reflection
// got no cull flip (kReflectionForcesReverseCull is false there).
constexpr int kDrawModeReflection = kDC3RetailModel ? 8 : 7;
// What culling a draw gets inside that mode. DC3's RndShader::CheckForceCull
// (rndobj/Shader.cpp) sets D3D cull mode 6 -- the reverse of a regular
// material's 2 -- for EVERY material while DrawMode() == 8, whatever its own
// cull: a two-sided (kCullNone) material loses its far faces in the mirror and
// a kCullBackwards one keeps mode 6 rather than being flipped. dc3-decomp
// follows that; rb3-xenon keeps the flip of the material's own cull it had.
constexpr bool kReflectionForcesReverseCull = kDC3RetailModel;

// ---- skin specular on the native light arm ---------------------------------
// rb3-xenon's frames used to fall to WgpuRnd's native (heuristic) light arm,
// which gave a skin material (shader_variation 1) an unnormalised Blinn lobe per
// light at the spec map's colour, power max(power * map alpha, 0.1). RB3's
// shipped X360 skin permutation (xbox_shaders `standard`
// 0000022040031037: skin, rim, normal + spec maps, 2 points; ucode
// instructions 27/28 colour and power, 56 lobe, 61-89 point lights, 64-107
// box reflection, 112 output) instead takes specular colour c2.rgb x
// map.rgb, p = max(c2.w x map.a, 0.5), n(p) = p / 2pi + 1 / pi, and lights
// with
//   points: c_i a_i n(p) sat(V . R_i)^p (1 - sat(V . N)^2)^2
//   approx: rimf^2 n(p) boxPow(reflect(-V, N), p),
//           rimf = (1 - sat(V . N)) (0.5 N.z + 0.5),
// the approx lights reaching the box faces with weight sat(+-L_axis)^2
// (BoxMapLighting::ApplyQueuedLights). So a skin texel facing the camera has
// almost no specular. SceneUniforms.skinSpecModel = 1 selects that form in the
// native arm (standard_wgsl.inc rb3SkinSpecDirectional and rb3SkinSpecPoint). rb3-xenon only: DC3 never
// reaches the native arm (WriteDC3Retail claims every frame). Since lane A62
// rb3-xenon lights through RB3's retail model (kRB3RetailLight); a skin
// material keeps this arm there (MatRetailTerms declines it), with the box
// faces the real c80..c85 instead of folded directionals and the points
// retail's two (standard_wgsl.inc rb3SkinRetailBox, rb3SkinSpecBox).
constexpr bool kSkinSpecRB3Xenon = !kDC3RetailModel;

// ---- camera select ---------------------------------------------------------
// DC3's RndCam::Select ends by handing the renderer the camera's viewport --
// mScreenRect scaled to the target, depth range mZRange -- through
// TheNgRnd.SetViewport (rndobj/Cam.cpp), so WgpuRnd receives it on every select.
constexpr bool kCamSelectSetsViewport = true;

// ---- flare point tests -----------------------------------------------------
// Whether WgpuRnd, after submitting a frame, blocks for the point-test answers
// of earlier frames that were not ready at world end (FinishPointTestReadback),
// so that every flare answer arrives one frame after its test, as retail
// DxRnd::DoPointTests' do. dc3-decomp (kDC3RetailModel) does not wait: on an
// integrated or phone GPU the wait is CPU time spent idle, and DC3 takes the
// occasional answer a frame late instead, with the flare's last answer standing
// in for it (gfx/PointTestStandIns.h) so its fade does not snap. rb3-xenon
// keeps the wait it had.
constexpr bool kPointTestWaitAfterSubmit = !kDC3RetailModel;

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
// A rect drawn while a texture is the render target is in that target's own
// pixels: DxRnd::DrawRect sets D3DRS_VIEWPORTENABLE 0 (and HalfPixelOffset 1),
// so its pre-transformed vertices are the bound surface's coordinates whatever
// the virtual screen is (dc3-decomp src/system/rnddx9/Rnd.cpp). rb3-xenon too:
// band.exe TU5's DxRnd::DrawRect (fn_82733538) sets its own transform at
// +0x500..+0x524 and calls SetRenderState_ViewportEnable(0) at +0x52C..+0x538,
// the same code, and MatSwap::Compose (fn_8229F268 +0x2DC/+0x2E4) sizes the
// outfit composite's rect from diffTex->mWidth/mHeight. Under the virtual-screen
// mapping a 512x256 head *_diffuse_output got only its top-left 40% x 36% (a
// 1024x512 torso 80% x 71%), the rest stayed at the target's clear colour, and
// the band's faces and limbs drew black.
constexpr bool kRectTargetPixels = true;

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
// np-look23 (a): DC3's post-process noise map (noise_mono.tex, 768 rows down
// any screen) is retiled to whole output rows and read at one fixed place in
// each texel, so it cannot beat against the output (gfx/NoiseRows.h). DC3's
// only: rb3-xenon, which also compiles this shape, keeps the authored uv.
constexpr bool kNoiseSnapsToOutputRows = kDC3RetailModel;
// lane A55: Rock Band 3's own Xbox 360 chain (gfx/RB3RetailPost, the shape the
// RB3-Wii consumer runs), for a consumer whose RndPostProc carries retail
// RB3's post state and not DC3's. Retail band.exe TU5 NgPostProc::DoPost
// (fn_82B89A08, NgPostProc vtable slot 3) runs DoBloom (fn_82B884A0: c6 =
// bloomColor * BloomIntensity(), no BloomLocFactor; no per-material bloom
// multiplier) and then ModulateColorXfm (the RndColorXfm 3x3 scaled by
// mColorModulation, +0x12C). So the detector is the colour modulation
// accessor in a shape without DC3's chain: rb3-xenon declares
// RndPostProc::GetColorModulation() under HX_NATIVE; dc3-decomp has DC3's
// chain (kDC3RetailPost) and keeps it. As on the RB3-Wii shape, the grade
// moves to Rnd::EndWorld (WgpuRnd::DoPostProcess -> FlushWorldPost), so the
// UI drawn after the world stays ungraded. DC3 already grades at EndWorld
// through its NgRnd::DoPostProcess.
template <class P, class = void> struct PostProcHasColorModulation : std::false_type {};
template <class P>
struct PostProcHasColorModulation<
    P, std::void_t<decltype(std::declval<const P &>().GetColorModulation())>> : std::true_type {};
constexpr bool kRetailPostChain =
    !kDC3RetailPost && PostProcHasColorModulation<RndPostProc>::value;
// Retail DxRnd::SavePreBuffer (DxRnd::DoWorldEnd's last call) resolves the
// world's colour with D3DRESOLVE_CLEARDEPTHSTENCIL (flags 0x300, ClearZ 0.0 =
// far under the default reverse Z), so everything drawn after the world (the UI
// and HUD) starts on a cleared depth buffer, while the world's depth stays
// readable in mFrontBufferDepth (the resolve just before, flags 0x14). Both
// DC3-lineage titles: rb3-xenon band.exe TU5 fn_8273B3B8 (+0xB8 li r4,0x300,
// +0xC0 the resolve), dc3-decomp rnddx9/Rnd_Xbox.cpp DxRnd::SavePreBuffer.
// WgpuRnd::SavePreBuffer reproduces it (Rnd_Wgpu.h, FrameDepthView).
constexpr bool kWorldEndClearsDepth = true;
// DC3_RETAIL_POST (rndshape/DC3PostChain.cpp): unset or anything else = 1 (the
// retail chain), "0" = 0 (PostProcPass's previous composite), "raw" = 2 (the
// scene ungraded), "mask" = 3 / "bloom" = 4 (the chain showing the bloom mask /
// the bloom term; for inspection), "grade" = 5 (the chain without bloom),
// "velocity" = 6 (the chain, showing DoVelocity's velocity target instead).
// Without kDC3RetailPost: with kRetailPostChain (rb3-xenon), the RB3 shape's
// MILO_RB3_RETAIL_POST grammar (RndShape_RB3Wii.h: unset = 1 the chain, "0"
// = PostProcPass's generic composite, "raw" 2, "mask" 3, "bloom" 4, "grade"
// 5, "beams" 6); else always 0.
int RetailPostMode();
// The frame's alpha carries the bloom mask this frame.
inline bool RetailBloomMaskActive() {
    if constexpr (kRetailPostChain) {
        const int m = RetailPostMode();
        return m == 1 || m == 3 || m == 4 || m == 6;
    } else if constexpr (!kDC3RetailPost) {
        return false;
    } else {
        const int m = RetailPostMode();
        return m == 1 || m == 3 || m == 4 || m == 6;
    }
}
// The post-process Rnd::DoPostProcess runs and Rnd::BeginDrawing takes the
// bloom colour from: the blacklight override (set_postproc_blacklight_override,
// the voice-control letterbox's PostProc_Blacklight.pp), else the override
// (set_postproc_override), else RndPostProc::Current(). dc3-decomp's Rnd
// carries the HX_NATIVE accessor; rb3-xenon (no blacklight field, and an
// override typed PostProcessor*) gets Current() (DC3PostChain.cpp).
RndPostProc *ActivePostProc();
// Rnd::DisablePP() ({rnd toggle_all_postprocs}): Rnd::DoPostProcess skips
// DoPost and DxRnd's FinishPostProcess still copies the scene through the
// option-free postprocess.ps, so the frame is the ungraded scene. False where
// the consumer's Rnd has no such accessor (DC3PostChain.cpp).
bool PostProcDisabled();
// This frame's RndVelocityBuffer::Draw result (NgPostProc::DoVelocity, run by
// Rnd::DoPostProcess just before the backend's post pass), taken once: false
// when Draw did not run or failed its camera test, else `ready` (bit 42 on),
// `scale` (c122) and the world camera. dc3-decomp's HX_NATIVE
// RndVelocityBuffer::TakeNativeFrame; false for a consumer without it.
bool TakeVelocityFrame(bool &ready, float &scale, RndCam *&cam);
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
// The frame at the output's own resolution (np-outres;
// GpuDeviceDesc::outputFollowsTarget): a 1920x1080 window or headless
// MILO_WIDTH x MILO_HEIGHT frame is drawn at 1920x1080, viewports, UI and the
// post chain's pixel measures laid out in the Rnd's virtual 1280x720 and scaled
// to it, pillar/letterboxed to keep 16:9. dc3-decomp (kDC3RetailPost) only:
// rb3-xenon keeps its 1280x720 frame scaled into the window. Within DC3 the
// look decides (LookSettings::outputFollowsTarget): Enhanced follows, Retail
// keeps the original's 1280x720 frame.
constexpr bool kOutputFollowsTarget = kDC3RetailPost;
// The standard shader's alpha fringe fade (standard_wgsl.inc: rgb *= a / 0.1
// for a < 0.1; PipelineManager::SetAlphaFringeFade). A native heuristic from
// the port's first shader (2026-03), with no counterpart in the image: the
// shipped standard pixel shaders write oC0 = texel * interpolated colour (key
// 0x10) or texel * colour + specular (0x20010) and never touch rgb by alpha,
// and NgMat::SetBasicState's blend factors do the rest. On DC3 it squared
// every low-alpha SrcAlpha layer (dclive's peak sky cubes, mat alpha 0.10-0.14
// times texel alpha, drew at less than half their light), erased every
// alpha-0 Add layer (DLV_PeakGlass_transparent.mat, alpha 0, Add, drew
// nothing), and already had to be switched off for Multiply/Darken (G-6) and
// colour-mod band texels (H-6). dc3-decomp (kDC3RetailModel), both looks;
// rb3-xenon keeps it.
constexpr bool kAlphaFringeFade = !kDC3RetailModel;
// The material's tex_wrap clamps its normal, specular and emissive maps as it
// does its diffuse: NgMat::SetBasicState (Mat_NG.cpp) sets the same
// sTexWrapClampModes[mTexWrap] on samplers 0 (diffuse), 6 (diffuse2),
// 1 (normal), 2 (specular) and 3 (emissive); only the detail normal map
// (sampler 14, SetRegularShaderConst) is set to wrap. Native sampled every map
// but the diffuse through one always-repeat sampler (MaterialSetup's
// mapSamplerDesc), so a clamped strip's emissive wrapped: streetside's peak
// stage edge (streetside_peek_blue.mat, kTexWrapClamp, emissive stripe10.tex)
// drew thin bright copies of its stripe above and below it that Xenia does not
// have, which the alpha fringe fade above had been hiding (the clamped
// diffuse's alpha is 0 there). A material with a detail normal or rim map
// keeps the repeating map sampler (one sampler serves them all).
// dc3-decomp (kDC3RetailModel) only; rb3-xenon keeps the repeating sampler.
constexpr bool kMapsFollowTexWrap = kDC3RetailModel;
// ---- end np-gfx-post ----
// Both consumers of this shape shade in gamma space, as both images do: no
// bitmap format DxRnd uses carries a gamma sign (rnddx9/Rnd.cpp
// D3DFormatForBitmap: DXT1/3/5, DXN, A8R8G8B8, A1R5G5B5, X8R8G8B8), the frame
// is a D3DFMT_A8R8G8B8 EDRAM surface (Rnd_Xbox.cpp), and no standard.ps
// permutation decodes or encodes, so the shader multiplies texel, material
// colour and lighting as stored and the target saturates the product
// (material.gammaShading).
//
// rb3-xenon (lane A60): RB3 TU5's DxRnd::D3DFormatForBitmap is band.exe
// fn_827333C8 (rb3-xenon rnddx9/Rnd.cpp, 100% matched); its format words are
// DXT1 0x1A200152, DXT3 0x1A200153, DXT5 0x1A200154, DXN 0x1A200171,
// A8R8G8B8 0x18280186 (the lis/ori pairs at fn_827333C8+0x34..+0x90), all with
// the four gamma sign fields (bits 9..16) zero, and RB3's shipped xbox_shaders
// standard.ps permutations contain no log/exp
// (milo-native-engine docs/native/dc3-backend-for-rb3-wii.md 10.1). rb3-xenon
// used to keep the linear model with the sRGB output encode, which drew its
// venues hazy and grey; that choice had been fitted against xenia frame dumps
// that xenia itself sRGB-encodes (xenia vulkan_command_processor_headless.cc
// srgb_lut; the guest front buffer is the _raw.ppm), i.e. against a reference
// one encode brighter than the image. Against the recovered front buffer the
// gamma-space model moves every frame metric toward retail
// (rb3-scout/a60/HANDOFF.md). Since lane A62 the light model rb3-xenon runs is
// retail's too (kRB3RetailLight), not the engine's heuristic arm.
constexpr bool kGammaSpaceShading = true;
// A Multiply-blend material is lit like any other (np-gfx-light round 3).
// NgMat::SetBasicState gives kBlendMultiply src ZERO / dest SRCCOLOR and fade
// params (1,1,1,1), and nothing in CalcShaderOpts or SetupAmbient looks at the
// blend, so the image shades it c0 * (c1 + box + points + proj) * texel and the
// target multiplies the frame by that (dc3-decomp rndobj/Mat_NG.cpp). The
// rollerrink floor's Rink*_overlay.mesh (Rink_lightCatcher.mat, no texture,
// Rink_lightCatcher.env: ambient 0.365 and mirrorball_spotlight_02's gobo) is
// such a "light catcher": it darkens the floor to 0.365 outside the light.
// MaterialSetup forced every Multiply material prelit (identity) for the old
// heuristic lighting, which brightened it; the DC3 retail model does not
// need that. rb3-xenon keeps the forced prelit.
constexpr bool kMultiplyBlendIsLit = kDC3RetailModel;
// Intensify doubles the texel's alpha with its colour in every draw, the world
// included (np-gfx-light round 4). NgMat::SetRegularShaderConst sets c5.y =
// intensify + 1, and every standard.ps permutation with option bit 53 scales
// the whole texel before it meets the interpolated colour: shipped
// standard_0020000000020010.ps is `mul r0, r0, c5.yyyy` then
// `mul oC0.___w, r0.wwww, r2.wwww`, where standard_0000000000020010.ps (the
// same key without bit 53) writes `r0.w * r2.w`. The engine applied the
// factor in the UI phase only (MaterialAlphaScale), so a world material such
// as streetside's peak stripes (streetside_peek_pink1..16.mat: SrcAlpha,
// intensify, alpha 0.57 down to 0.025, alpha_write) drew half as opaque and
// wrote half the bloom mask retail does. rb3-xenon keeps the UI-only rule.
constexpr bool kIntensifyScalesAlpha = kDC3RetailModel;
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
// The material's half of BloomMaskScale (NgMat::AllowHDR, dc3-decomp
// rndobj/Mat_NG.cpp), without the frame's (retail post on, a current
// RndPostProc, no environment fade): whether the mask can ever be written for
// it. The load-time pipeline warm asks it, as the frame's half is not
// knowable at load.
// AllowsBloomMaskFor: the same with the blend and alpha write given (a state
// a property animation keys) instead of read from the material.
template <class M>
inline bool AllowsBloomMaskFor(const M &m, int blend, bool alphaWrite) {
    using Raw = std::remove_cv_t<std::remove_pointer_t<std::decay_t<M>>>;
    if constexpr (kRetailPostChain && sizeof(Raw) > 0) {
        // RB3's NgMat::AllowHDR (rb3-xenon rndobj/Mat_NG.cpp): not
        // alpha-blended, alpha-cut or alpha-writing.
        if (!m) return false;
        return !(blend == (int)Raw::kBlendSrcAlpha || blend == (int)Raw::kBlendSrcAlphaAdd ||
                 blend == (int)Raw::kPreMultAlpha || m->GetAlphaCut() || alphaWrite);
    } else if constexpr (!kDC3RetailPost || !MatHasBloomMultiplier<Raw>::value) {
        (void)m;
        (void)blend;
        (void)alphaWrite;
        return false;
    } else {
        if (!m) return false;
        return !(blend == (int)Raw::kBlendSrcAlpha || blend == (int)Raw::kBlendSrcAlphaAdd ||
                 blend == (int)Raw::kPreMultAlpha || m->GetAlphaCut() || alphaWrite);
    }
}
template <class M>
inline bool AllowsBloomMask(const M &m) {
    if (!m) return false;
    return AllowsBloomMaskFor(m, (int)m->GetBlend(), (bool)m->GetAlphaWrite());
}
template <class M>
inline float BloomMaskScale(const M &m, bool envFadeOutCancels = true) {
    using Raw = std::remove_cv_t<std::remove_pointer_t<std::decay_t<M>>>;
    if constexpr (kRetailPostChain && sizeof(Raw) > 0) {
        // RB3's pseudo-HDR mask (standard.ps bit 22), the positive scale the
        // standard shader reads as the RB3 mask: c7's 1/threshold. The same
        // environ fade-out test as CalcShaderOpts (!fadeOut).
        if (!m || !RetailBloomMaskActive()) return 0.0f;
        const RndPostProc *pp = RndPostProc::Current();
        if (!pp) return 0.0f;
        if (envFadeOutCancels) {
            const RndEnviron *env = RndEnviron::Current();
            if (env && env->FadeOut() && env->FadeEnd() != env->FadeStart()) return 0.0f;
        }
        if (!AllowsBloomMask(m)) return 0.0f;
        return BloomLumaScale(pp);
    } else if constexpr (!kDC3RetailPost || !MatHasBloomMultiplier<Raw>::value) {
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
        if (!AllowsBloomMask(m)) return 0.0f;
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
// The display type the console corrects that ramp for before scan-out
// (D3DDevice_SetGammaRamp -> D3D::GammaRampCorrected, gfx/DisplayRamp.h):
// DisplayRamp::kDisplayTV by default, in every look (the console on an HDMI
// or component TV); kDisplaySRGB (the ramp alone, a VGA-cabled console) with
// DC3_DISPLAY_GAMMA=srgb. rb3-xenon: kDisplaySRGB.
int DisplayGammaType();
// {rnd native_look display_tv|display_srgb|display_off|display_rule}: force a
// display type (kDisplayTV, kDisplaySRGB), 0 = no ramp, -1 = back to
// DC3_DISPLAY_GAMMA / the default. Inspection only.
void SetDisplayTypeForInspection(int type);
int DisplayTypeForInspection();
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
// shadowEnabled = 2 + its atlas tile; WgpuRnd::EndDrawing then calls
// CollectProjShadowCasterSets and draws each light's casters into its tile of
// ShadowPass's map, which the next frame samples.
// rb3-xenon keeps the generic pre-pass. Defined in DC3SceneLighting.cpp.
constexpr bool kGenericShadowPass = !kDC3RetailModel;
// A mesh draw that cannot change a pixel is not sent (DrawChangesNothing,
// platform/MeshPipelineKey.h). dc3-decomp; rb3-xenon keeps every draw, as its
// per-draw log's readers expect.
constexpr bool kSkipInvisibleDraws = kDC3RetailModel;

// The character self-shadow (standard.ps option bit 19). Character::
// DrawShowing calls RndShadowMap::PrepShadow, which selects a light camera
// whose target is a 512x512 RndTex::kShadowMap texture, draws the character's
// shadow group into it in Rnd::kDrawExtrude (the shadowmap shader, culling
// counter-clockwise: CheckForceCull's mode 6, the reverse of a regular
// material's 2, so the map holds the casters' far faces), reselects the view
// camera and calls TheRnd.SetShadowMap(tex, lightCam, colour); the
// character's own draws then read the map (GetShadowCam() non-null) until
// EndShadow. The native form keeps that order in the same frame: the light
// camera's pass is WgpuRnd's texture pass, its depth attachment is the map,
// and the scene block written for the receivers carries the light camera's
// clip transform and c107/c108 (SceneUniforms.selfShadow*), with the map at
// group 0 binding 1. dc3-decomp only; rb3-xenon (which also runs PrepShadow)
// keeps its renders as they were.
constexpr bool kSelfShadowMap = kDC3RetailModel;
// The draw mode PrepShadow draws the casters in (Rnd::kDrawExtrude).
constexpr int kSelfShadowCasterDrawMode = 2;
// The texture type of PrepShadow's map (RndTex::kShadowMap): its depth target
// is created sampleable.
constexpr int kSelfShadowTexType = 0x42;
// TheRnd.GetShadowCam() (null outside PrepShadow..EndShadow), and the pixel
// constants NgRnd::SetShadowMap uploaded with it: c107 = kPS_ShadowColor
// (0x6B), c108 = kPS_ShadowCamDir (0x6C). Defined in Rnd_Wgpu.cpp.
RndCam *SelfShadowCam();
const float *CapturedPConstant(int reg);

// Rnd::BeginDrawing selects mDefaultEnv every frame (rndobj/Rnd.cpp), so a
// draw that selects no environ of its own -- the UI after EndWorld -- gets the
// default environ. WgpuRnd::BeginDrawing used to select it only when none was
// current, which left the last frame's environ (a venue's) current. DC3
// follows the image; rb3-xenon keeps the native rule (its renders are pinned
// byte-identical).
constexpr bool kSelectDefaultEnvEachFrame = kDC3RetailModel;
// Every projected light with shadow objects that this frame's environs
// published (up to kMaxProjShadowMaps, in first-published order), one set per
// light with its atlas tile; ends the frame (the next frame's receivers read
// these maps). Returns the number of sets.
constexpr int kMaxProjShadowMaps = 4;
int CollectProjShadowCasterSets(std::vector<ProjShadowCasters> &out);
// The same, keeping only the first set (tile 0). For callers that drive one
// light at a time (tests).
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
