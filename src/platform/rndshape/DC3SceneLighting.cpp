// DC3SceneLighting.cpp — DC3's own light model behind the rndshape seam
// (MILO_ENGINE_GPU_BACKEND=dc3, MILO_ENGINE_RNDOBJ_SHAPE=dc3). Built for both
// consumers of the DC3 shape; every body below is selected by
// rndshape::kDC3RetailModel (RndShape_DC3.h), which is true for dc3-decomp and
// false for rb3-xenon. rb3-xenon takes the bodies under
// rndshape::kRB3RetailLight instead: RB3's own X360 model, the engine's
// retail form 1 (WriteRB3XenonRetail / FillRB3XenonApprox, lane A62); before
// that it got `return false` / no-ops and the heuristic arm in
// WgpuRnd::WriteSceneUniforms.
//
// What the Xbox 360 image does, from the matched decomp (dc3-decomp
// src/system/rndobj/Env_NG.cpp, BoxMap.cpp, Mat_NG.cpp, Shader.cpp; the
// functions named below are 100% matched unless said otherwise) and the
// shipped xbox_shaders `standard` permutations; the evidence is in
// docs/native/dc3-retail-lighting.md.
//
//  * ambient: c1 = the environ's ambient, unscaled (NgMat::SetupAmbient),
//    (1,1,1,1) for a material with use_environ off;
//  * real lights (NgEnviron::Select, 98.6%: the residual is store order):
//    the first THREE lit point lights in mLightsReal order (Showing and a
//    non-zero packed colour), c62+i = (position, 1 / (falloffStart - range))
//    and c66+i = (colour, -range * that), or (.., 0) and (.., 1) when
//    falloffStart >= range: full strength inside falloffStart, linear to zero
//    at range; at most one projected light in slot 3 (WriteDC3ProjShadow);
//  * approx lights: NgEnviron::UpdateApproxLighting folds mLightsApprox and
//    RndEnviron::sGlobalLighting into the six-face box map c80..c85 at the
//    position Select was given -- the character's origin for a Character
//    (Character::DrawLodOrShadow's RndEnvironTracker), the RndDir's origin for
//    an RndDir with its own environ (RndDir::DrawShowing, so static venue
//    geometry gets one), none where an environ is selected with a null
//    position (Crowd, Spotlight, TexRenderer: then points and spots in the
//    approx set are skipped and only directionals and fake spots land). The
//    native port runs
//    that same decomp code, and WgpuShaderMgr keeps the registers it uploads;
//    FillMeshApproxLighting copies them into each draw;
//  * standard.vs/ps: colour * (ambient + box(N) + points) * texel, per vertex
//    or per pixel, no clamp before the 8-bit target (gamma space, see
//    rndshape::kGammaSpaceShading);
//  * fog (option bit 18): f = sat(2^(-2.88539 * clip.w * c91.y)),
//    c91.y = 1 / (end - start), out = mix(fogColour, colour, f) (permutation
//    0x70010) -- exponential, not the native linear ramp.
//  * no AO: the shipped cache has the AO bit (38) only on billboard
//    permutations, none with a diffuse map.

#include "platform/rndshape/RndShape.h"
#include "gfx/UniformStructs.h"
#include "platform/LookProfile.h"

#include "char/Character.h"
#include "obj/Dir.h"
#include "rndobj/Dir.h"
#include "rndobj/Group.h"
#include "rndobj/Cam.h"
#include "rndobj/Lit.h"
#include "rndobj/Mat.h"
#include "rndobj/Mesh.h"

#include <cstring>

namespace {

// ---- the projected shadow light (projected_blend 1) -------------------------
// NgEnviron::Select takes the first kFakeSpot of mLightsReal that
// CheckProjLight accepts into slot 3; SetProjLightRegisters then puts
// c65 = -WorldXfm().m.y, c69 = (colour, 1), c95..c97 = RndLight::Projection()
// (4x3, transposed) and the light's shadow map in tf5. With projected_blend 1
// (shadow_projected) standard.ps 0x100010030011 reads
//   uv = (c95 . p, c96 . p) / (c97 . p),  s = tf5(uv).a (border 0 outside),
//   out = (c1 c0 + box(N) c0 (1 - 0.75 s c69 sat(c65 . N))) * texel,
// and NgLight::CheckShadowMap fills tf5 by drawing the light's shadow objects
// (mShadowOverride, else mShadowObjects) in kDrawOcclusion into a 256x256
// target cleared to 0 and blurring it twice with a 5-tap kernel
// (NgLight::BlurShadowRT). Both are 100% matched or listed in the doc.
//
// Native: the casters are drawn at EndDrawing into ShadowPass's depth map at
// depth 0 over a clear of 1, with a clip transform built from the same
// Projection() rows so that the rasteriser lands each caster where the
// receiver's uv reads it: clip = (2 r0 - r2, r2 - 2 r1, 0, r2) . p gives
// u = 0.5 x/w + 0.5 = r0.p / r2.p and v = 0.5 - 0.5 y/w = r1.p / r2.p. The
// receivers sample it the next frame (standard_wgsl.inc dc3ProjShadow).
//
// Every projected light with shadow objects gets a map, as on retail (each
// NgLight owns its target). Houseparty is the venue that needs more than one:
// a2_spotlight.lit (default_environment.env, projected_blend 0, a gobo with
// five shadow objects) and ShadowDirLight01.lit (Shadows.env, projected_blend
// 1, the dancers' floor shadow) are both live in one frame (np-gfx-light
// round 3 probe). With a single map the first one published took it and the
// floor shadow drew none. The maps share ShadowPass's depth texture as a 2x2
// atlas; a light's tile is its index in the frame that drew it.
struct ProjShadowState {
    RndLight *frameLights[rndshape::kMaxProjShadowMaps] = {};   // this frame's; never kept across frames
    float frameClips[rndshape::kMaxProjShadowMaps][16] = {};
    // The camera frustum current when the light was first selected this frame
    // (retail: RndCam::Current() during NgLight::CheckShadowMap), which every
    // nested caster draw is culled against; hasFrustum false = no camera.
    Frustum frameFrustums[rndshape::kMaxProjShadowMaps];
    bool frameHasFrustum[rndshape::kMaxProjShadowMaps] = {};
    int frameCount = 0;
    const void *mapLights[rndshape::kMaxProjShadowMaps] = {};   // whose casters each tile holds (identity only)
    int mapCount = 0;
};
ProjShadowState sProj;

// RndLight::mShadowObjects is protected and has no accessor; a member pointer
// named through a derived class reaches it without touching the decomp.
template <class L>
struct LightShadowObjects : L {
    static auto Member() { return &LightShadowObjects::mShadowObjects; }
};

// The cone radii, likewise protected with setters only.
template <class L>
struct LightConeRadii : L {
    static auto Top() { return &LightConeRadii::mTopRadius; }
    static auto Bot() { return &LightConeRadii::mBotRadius; }
};

// The gobo (projected_blend 0's tf10) of the light WriteSceneLighting last
// published; null for blend 1 or no projected light.
RndTex *sFrameGobo = nullptr;

template <class L>
bool LightHasShadowObjects(L *l) {
    return (l->GetShadowOverride() && l->GetShadowOverride()->size() != 0) ||
        l->ShadowObjectsSize() != 0;
}

// NgEnviron.cpp CheckProjLight (minus its CheckShadowMap call).
template <class L>
bool ProjLightAccepted(L *l) {
    if (!l->Showing()) return false;
    if (l->GetProjectedBlend() == 0 && l->GetTexture() == nullptr) return false;
    if (l->GetProjectedBlend() == 1) {
        const bool hasOverride = l->GetShadowOverride() && l->GetShadowOverride()->size() != 0;
        if (!hasOverride && l->ShadowObjectsSize() == 0) return false;
    }
    return l->GetColor().Pack() != 0;
}

template <class L>
void ProjShadowClip(L *l, float out[16]) {
    const Transform p = l->Projection();
    const float r0[4] = {p.m.x.x, p.m.y.x, p.m.z.x, p.v.x};
    const float r1[4] = {p.m.x.y, p.m.y.y, p.m.z.y, p.v.y};
    const float r2[4] = {p.m.x.z, p.m.y.z, p.m.z.z, p.v.z};
    for (int c = 0; c < 4; c++) {   // column-major: out[c * 4 + row]
        out[c * 4 + 0] = 2.0f * r0[c] - r2[c];
        out[c * 4 + 1] = r2[c] - 2.0f * r1[c];
        out[c * 4 + 2] = 0.0f;
        out[c * 4 + 3] = r2[c];
    }
}

// The transform the casters are drawn with: NgLight::SetShadowTransforms
// (100% matched), not Projection(). It takes the light's true inverse world
// transform (Invert), swaps y and z, and applies the cone projection, so
// clip = (xL, zL, yL / range, top + (bot - top) yL / range) with (xL, yL, zL)
// the light-local position. A 256x256 viewport maps clip x/w, y/w to the
// target as the receivers' bias does, so in this atlas's clip convention the
// rows are (xL, zL, 0, top + slope yL).
//
// Projection() builds the receivers' uv from the raw rows of WorldXfm (and
// multiplies in mTextureXfm). For an orthonormal light with an identity
// texture_xfm the two agree. They differ when the light's transform is
// scaled: dclive's a2_spotlight.lit has world scale (1, 0.86, 0.86), so on
// the Xbox the casters land in the map at a different place and size than
// the receivers read, and its dancers' shadows sit about 25 px (720p) further
// up the floor than the geometry puts them, which is where Xenia draws them
// (docs/native/dc3-retail-lighting.md §15). False (and the caller keeps the
// Projection() clip) for a light of zero range, where Projection() is the
// identity and SetShadowTransforms would divide by zero.
template <class L>
bool ProjShadowCasterClip(L *l, float out[16]) {
    const float range = l->Range();
    if (range == 0.0f) return false;
    Transform inv;
    Invert(l->WorldXfm(), inv);
    const float invRange = 1.0f / range;
    const float top = l->*LightConeRadii<L>::Top();
    const float slope = (l->*LightConeRadii<L>::Bot() - top) * invRange;
    const float xr[4] = {inv.m.x.x, inv.m.y.x, inv.m.z.x, inv.v.x};
    const float yr[4] = {inv.m.x.y, inv.m.y.y, inv.m.z.y, inv.v.y};
    const float zr[4] = {inv.m.x.z, inv.m.y.z, inv.m.z.z, inv.v.z};
    for (int c = 0; c < 4; c++) {   // column-major: out[c * 4 + row]
        out[c * 4 + 0] = xr[c];
        out[c * 4 + 1] = zr[c];
        out[c * 4 + 2] = 0.0f;
        out[c * 4 + 3] = slope * yr[c] + (c == 3 ? top : 0.0f);
    }
    // The image rasterises the casters with D3D9's pixel centres: the device
    // leaves D3DRS_HALFPIXELOFFSET at 0 for every draw except DxRnd::DrawRect
    // (and DrawRectDepth / the point test), which set it to 1 for the rect and
    // back to 0. So texel i of the 256x256 target holds the caster at x = i,
    // where the receivers' bilinear read puts texel i at i + 0.5: the map the
    // receivers see is half a texel (+u, +v) from the geometry. BlurShadowRT's
    // two passes are DrawRects, so they add nothing. WebGPU samples coverage
    // at pixel centres, so the same map needs the geometry moved by half a
    // 256-texel: +1/256 of w in x and -1/256 in y (clip y is up, v is down).
    // MEASURED against Xenia (§15): streetside's boombox shadow centroid is
    // 12 px off without it and 1 px with it.
    const float halfTexel = 1.0f / 256.0f;   // 0.5 texel of 256, in clip units
    for (int c = 0; c < 4; c++) {
        out[c * 4 + 0] += halfTexel * out[c * 4 + 3];
        out[c * 4 + 1] -= halfTexel * out[c * 4 + 3];
    }
    return true;
}

template <class E>
void WriteDC3ProjShadow(SceneUniforms &s, E *env, RndCam *cam) {
    using LightT = std::remove_pointer_t<decltype(*env->LightsReal().begin())>;
    LightT *proj = nullptr;
    for (auto it = env->LightsReal().begin(); it != env->LightsReal().end(); ++it) {
        LightT *l = *it;
        if (l && l->GetType() == LightT::kFakeSpot && ProjLightAccepted(l)) {
            proj = l;
            break;
        }
    }
    if (!proj) return;
    const int blend = proj->GetProjectedBlend();
    float clip[16];
    ProjShadowClip(proj, clip);
    memcpy(s.lightViewProj, clip, sizeof(clip));
    const Transform &xfm = proj->WorldXfm();
    s.projLightDir[0] = -xfm.m.y.x;   // c65 = -m.y, used as L directly
    s.projLightDir[1] = -xfm.m.y.y;
    s.projLightDir[2] = -xfm.m.y.z;
    s.projLightDir[3] = 0.0f;
    const Hmx::Color &c = proj->GetColor();
    s.projLightColor[0] = c.red;
    s.projLightColor[1] = c.green;
    s.projLightColor[2] = c.blue;
    // w: which projected form the receivers run (standard_wgsl.inc dc3ProjLight).
    //   1 = projected_blend 1, the multiply (shadow) form, 0x100010030011;
    //   2 = projected_blend 0, the additive gobo form, 0x10030011.
    // Any other blend is neither: CalcShaderOpts sets mProjLightMultiply only
    // for 1, so the additive permutation runs, and it is drawn as blend 0.
    s.projLightColor[3] = blend == 1 ? 1.0f : 2.0f;
    sFrameGobo = blend == 1 ? nullptr : proj->GetTexture();
    // The coverage map exists only for a light with shadow objects; retail
    // binds the FlatNormal default (alpha 0: no coverage) otherwise
    // (NgLight::CheckShadowMap, Rnd::CreateDefaultTexture row 3).
    if (!LightHasShadowObjects(proj)) return;
    // 2 + tile: the tile holds last frame's draw of this light's casters.
    s.shadowEnabled = 0.0f;
    for (int i = 0; i < sProj.mapCount; i++) {
        if (sProj.mapLights[i] == proj) s.shadowEnabled = 2.0f + (float)i;
    }
    bool queued = false;
    for (int i = 0; i < sProj.frameCount; i++) queued = queued || sProj.frameLights[i] == proj;
    if (!queued && sProj.frameCount < rndshape::kMaxProjShadowMaps) {
        sProj.frameLights[sProj.frameCount] = proj;
        float casterClip[16];
        if (!ProjShadowCasterClip(proj, casterClip)) memcpy(casterClip, clip, sizeof(clip));
        memcpy(sProj.frameClips[sProj.frameCount], casterClip, sizeof(casterClip));
        sProj.frameHasFrustum[sProj.frameCount] = cam != nullptr;
        if (cam) sProj.frameFrustums[sProj.frameCount] = cam->WorldFrustum();
        sProj.frameCount++;
    }
}

// T, spelled so that it depends on D: names used through it are looked up
// only when the enclosing template is instantiated.
template <class T, class D> struct DependentT { using type = T; };

// Character's floor-shadow members are protected and have no accessor; member
// pointers named through a derived class reach them without touching the decomp.
template <class C>
struct CharShadowMembers : C {
    static auto Shadow() { return &CharShadowMembers::mShadow; }
    static auto DrawMode() { return &CharShadowMembers::mDrawMode; }
};

// The meshes a caster draws in kDrawOcclusion, by the image's own draw calls
// (NgLight::RenderShadows calls DrawShowing on each shadow object; everything
// below it goes through RndDrawable::Draw, which skips a hidden drawable):
//  * a mesh draws itself;
//  * RndGroup::DrawShowing and RndDir::DrawShowing draw their draw list
//    (mDraws), not every object they own;
//  * Character::DrawLod, in kDrawOcclusion, draws only with mFloorShadow set
//    and the opaque bit of its draw mode, and then DrawLodOrShadow(lod, 4)
//    draws the `shadow` group when it has one, else DrawOpaque: the
//    character's own draw list plus the current LOD's opaque list. Prop
//    meshes nobody lists, the other LODs and the translucent lists are not
//    casters. (All four DC3 dancers carry a shadow group of *_lod meshes.)
// Every nested draw goes through RndDrawable::Draw, which skips a drawable
// whose world sphere is wholly outside RndCam::Current()'s frustum, and
// RenderShadows does not select a light camera: the casters are culled
// against the view being drawn, so a dancer out of view casts no shadow onto
// the floor in view. `cull` is that frustum (null: no camera, no cull).
// Templated on the mesh type so the Character branch is instantiated only for
// dc3-decomp: rb3-xenon compiles this TU with its own Character, whose shadow
// is a single RndGroup.
template <class Mesh>
void AddOcclusionDraw(RndDrawable *d, std::vector<RndMesh *> &out, int depth, const Frustum *cull) {
    if (!d || depth > 8 || !d->Showing()) return;
    // RndDrawable::Draw: a nested draw whose world sphere is wholly outside
    // the current camera's frustum draws nothing. The shadow object itself
    // (depth 0) is drawn with DrawShowing and is not tested.
    if (depth > 0 && cull) {
        Sphere sphere;
        if (d->MakeWorldSphere(sphere, false) && sphere > *cull) return;
    }
    if (Mesh *m = dynamic_cast<Mesh *>(d)) {
        out.push_back(m);
        return;
    }
    if constexpr (rndshape::kDC3RetailModelFor<Mesh>) {
        using CharT = typename DependentT<Character, Mesh>::type;
        if (CharT *c = dynamic_cast<CharT *>(d)) {
            const auto mode = c->*CharShadowMembers<CharT>::DrawMode();
            if (!c->FloorShadow() || !(mode & CharT::kCharDrawOpaque)) return;
            const auto &shadow = c->*CharShadowMembers<CharT>::Shadow();
            if (shadow.size() != 0) {
                for (auto it = shadow.begin(); it != shadow.end(); ++it)
                    AddOcclusionDraw<Mesh>(it->Obj(), out, depth + 1, cull);
                return;
            }
            for (int i = 0; i < c->NumDraws(); i++)
                AddOcclusionDraw<Mesh>(c->GetDraw(i), out, depth + 1, cull);
            if (c->NumLods() != 0) {
                int lod = c->LastLod();
                lod = lod < 0 ? 0 : (lod >= c->NumLods() ? c->NumLods() - 1 : lod);
                const auto &opaque = c->GetLod(lod).mOpaque;
                for (auto it = opaque.begin(); it != opaque.end(); ++it)
                    AddOcclusionDraw<Mesh>(it->Obj(), out, depth + 1, cull);
            }
            return;
        }
    }
    if (RndGroup *g = dynamic_cast<RndGroup *>(d)) {
        for (RndDrawable *child : g->Draws()) AddOcclusionDraw<Mesh>(child, out, depth + 1, cull);
        return;
    }
    if (RndDir *dir = dynamic_cast<RndDir *>(d)) {
        for (int i = 0; i < dir->NumDraws(); i++)
            AddOcclusionDraw<Mesh>(dir->GetDraw(i), out, depth + 1, cull);
    }
}

template <class L>
bool CollectDC3Casters(rndshape::ProjShadowCasters &out, L *light, const Frustum *cull) {
    auto *list = light->GetShadowOverride();
    if (!list || list->size() == 0) list = &(light->*LightShadowObjects<L>::Member());
    for (auto it = list->begin(); it != list->end(); ++it)
        AddOcclusionDraw<RndMesh>(*it, out.meshes, 0, cull);
    return true;
}

template <class E>
void WriteDC3Retail(SceneUniforms &s, E *env, RndCam *cam) {
    s.retailLighting = 2.0f;      // 2: DC3's form of the retail model
    s.specularAA = milo::Look().specularAA ? 1.0f : 0.0f;  // LookSettings::specularAA
    s.pointFalloffMode = 2.0f;
    s.venueHighlightLumaMode = 0.0f;
    s.numLights = 0.0f;
    s.numPointLights = 0.0f;
    s.numProjLights = 0.0f;
    s.aoStrength = 0.0f;
    s.shadowEnabled = 0.0f;
    s.projLightColor[3] = 0.0f;   // no projected light
    sFrameGobo = nullptr;
    s.ambientColor[0] = s.ambientColor[1] = s.ambientColor[2] = 0.0f;
    s.ambientColor[3] = 1.0f;
    if (!env) return;

    // RndEnviron::AmbientColor reads mAmbientFogOwner.
    if (env->AmbientFogOwner()) {
        const Hmx::Color &amb = env->AmbientColor();
        s.ambientColor[0] = amb.red;
        s.ambientColor[1] = amb.green;
        s.ambientColor[2] = amb.blue;
        s.ambientColor[3] = 1.0f;
        // NgEnviron::Select: c91 = (end, 1 / (end - start), 0, 1), c90 = colour.
        if (env->FogEnable()) {
            s.fogEnabled = 2.0f;   // 2: the exponential form above
            s.fogStart = env->FogStart();
            s.fogEnd = env->FogEnd();
            const Hmx::Color &fc = env->FogColor();
            s.fogColor[0] = fc.red;
            s.fogColor[1] = fc.green;
            s.fogColor[2] = fc.blue;
        }
    }

    // NgEnviron::Select / SetPointLightRegisters, slot order = list order.
    int numPoint = 0;
    for (auto it = env->LightsReal().begin(); it != env->LightsReal().end() && numPoint < 3;
         ++it) {
        auto *L = *it;
        if (!L || L->GetType() != std::remove_pointer_t<decltype(L)>::kPoint) continue;
        if (!L->Showing() || L->GetColor().Pack() == 0) continue;
        const Transform &xfm = L->WorldXfm();
        float slope = 0.0f, rangeScale = 1.0f;
        if (L->FalloffStart() < L->Range()) {
            slope = 1.0f / (L->FalloffStart() - L->Range());
            rangeScale = -(L->Range() * slope);
        }
        const Hmx::Color &c = L->GetColor();
        s.pointLightPos[numPoint][0] = xfm.v.x;
        s.pointLightPos[numPoint][1] = xfm.v.y;
        s.pointLightPos[numPoint][2] = xfm.v.z;
        s.pointLightPos[numPoint][3] = slope;
        s.pointLightColors[numPoint][0] = c.red;
        s.pointLightColors[numPoint][1] = c.green;
        s.pointLightColors[numPoint][2] = c.blue;
        s.pointLightColors[numPoint][3] = rangeScale;
        s.pointLightRanges[numPoint] = L->Range();
        numPoint++;
    }
    s.numPointLights = (float)numPoint;

    WriteDC3ProjShadow(s, env, cam);

    // NgEnviron::Select: PS c109..c111 = the colour transform when
    // use_color_adjust (option bit 21, CalcShaderOpts reads the environ's
    // UseColorAdjust for every standard draw); written as the matrix columns,
    // out.c = rgb . m.<row>.c + v.c, the same layout RB3's uses.
    s.colorAdjust = env->UseColorAdjust() ? 1.0f : 0.0f;
    if (s.colorAdjust > 0.5f) {
        const Transform &cx = env->ColorXfm();
        const float *m0 = &cx.m.x.x;
        const float *m1 = &cx.m.y.x;
        const float *m2 = &cx.m.z.x;
        const float t[3] = {cx.v.x, cx.v.y, cx.v.z};
        for (int c = 0; c < 3; c++) {
            s.colorXfm[c][0] = m0[c];
            s.colorXfm[c][1] = m1[c];
            s.colorXfm[c][2] = m2[c];
            s.colorXfm[c][3] = t[c];
        }
    }
}

template <class E, class M>
void FillDC3Approx(E *env, M *mesh, float box[6][4], float retail[4]) {
    // retail.z: the material receives projected lights (CalcShaderOpts:
    // mNumProj = GetRecvProjLights() && NumLights_Proj() > 0; the environ
    // half is SceneUniforms.shadowEnabled).
    retail[2] = (mesh->Mat() && mesh->Mat()->GetRecvProjLights()) ? 1.0f : 0.0f;
    retail[0] = 0.0f;   // no AO (see the file comment)
    // CalcShaderOpts: approx (bit 17) = UseEnviron && NumLights_Approx > 0;
    // NgEnviron::UpdateApproxLighting raises the count to 1 whenever there is
    // a real light. The material half (use_environ) is the shader's.
    retail[1] = env ? (float)env->NumLights_Approx() : 0.0f;
    // kVS_BoxMapLight0 + i (0x50..0x55), faces {+X,-X,+Y,-Y,+Z,-Z}.
    for (int i = 0; i < 6; i++) {
        const float *v = rndshape::CapturedVConstant(0x50 + i);
        box[i][0] = v[0];
        box[i][1] = v[1];
        box[i][2] = v[2];
        box[i][3] = 1.0f;
    }
}

// ---- rb3-xenon: RB3's own Xbox 360 light model (rndshape::kRB3RetailLight) --
// The engine's retail form 1, the one the shader's retailLighting 1 branch
// runs (docs/native/dc3-backend-for-rb3-wii.md section 9), fed from the
// consumer's own decomp rather than reconstructed:
//  * c1 = the environ's ambient, unscaled, alpha included (NgMat::SetupAmbient;
//    a material with use_environ off gets 1 through rndshape::MatUnlit, and
//    c1.w = 1 through rndshape::MatAmbientIsEnviron, gfx/AmbientAlpha.h);
//  * real lights: NgEnviron::Select (band.exe TU5 fn_82B86738) keeps at most
//    TWO point lights, in mLightsReal order, each Showing with a non-zero
//    packed colour (CheckPointLight / SetPointLightRegisters): c64+i =
//    (position, 1 / (falloffStart - range)), c67+i = (colour, -range * that),
//    or (.., 0) / (.., 1) when falloffStart >= range;
//  * c109..c111 = the colour transform under use_color_adjust, and the AO
//    strength c24 under mAOEnabled (the shader's AO also needs the mesh's
//    HasAOCalc, FillRB3XenonApprox);
//  * the box map: per mesh, below.
// Fog keeps the form the heuristic arm wrote (linear, fogEnabled 1); RB3's
// fog microcode was not read for this lane.
template <class E>
void WriteRB3XenonRetail(SceneUniforms &s, E *env) {
    s.retailLighting = 1.0f;      // 1: RB3's form of the retail model
    s.pointFalloffMode = 2.0f;    // retail's linear falloff
    s.venueHighlightLumaMode = 0.0f;
    s.numLights = 0.0f;           // no directionals: approx lights are the box map
    const Hmx::Color &amb = env->AmbientColor();
    s.ambientColor[0] = amb.red;
    s.ambientColor[1] = amb.green;
    s.ambientColor[2] = amb.blue;
    // c1.w: the environ's ambient alpha (NgMat::SetupAmbient fn_824A9090
    // +0x38..+0x58 loads all four words). The shader multiplies it into the
    // output alpha of a material whose c1 is the environ's (gfx/AmbientAlpha.h);
    // RB3's UI fades whole groups this way (movie_pause.milo fade.env).
    s.ambientColor[3] = amb.alpha;
    if (env->FogEnable()) {
        s.fogEnabled = 1.0f;
        s.fogStart = env->FogStart();
        s.fogEnd = env->FogEnd();
        const Hmx::Color &fc = env->FogColor();
        s.fogColor[0] = fc.red;
        s.fogColor[1] = fc.green;
        s.fogColor[2] = fc.blue;
    }
    int numPoint = 0;
    for (auto it = env->LightsReal().begin(); it != env->LightsReal().end() && numPoint < 2;
         ++it) {
        auto *L = *it;
        if (!L || L->GetType() != std::remove_pointer_t<decltype(L)>::kPoint) continue;
        if (!L->Showing() || L->GetColor().Pack() == 0) continue;
        const Transform &xfm = L->WorldXfm();
        float slope = 0.0f, rangeScale = 1.0f;
        if (L->FalloffStart() < L->Range()) {
            slope = 1.0f / (L->FalloffStart() - L->Range());
            rangeScale = -(L->Range() * slope);
        }
        const Hmx::Color &c = L->GetColor();
        s.pointLightPos[numPoint][0] = xfm.v.x;
        s.pointLightPos[numPoint][1] = xfm.v.y;
        s.pointLightPos[numPoint][2] = xfm.v.z;
        s.pointLightPos[numPoint][3] = slope;
        s.pointLightColors[numPoint][0] = c.red;
        s.pointLightColors[numPoint][1] = c.green;
        s.pointLightColors[numPoint][2] = c.blue;
        s.pointLightColors[numPoint][3] = rangeScale;
        s.pointLightRanges[numPoint] = L->Range();
        numPoint++;
    }
    s.numPointLights = (float)numPoint;
    s.aoStrength = (env->AOEnabled() && env->AOStrength() > 0.003f) ? env->AOStrength() : 0.0f;
    s.colorAdjust = env->UseColorAdjust() ? 1.0f : 0.0f;
    if (s.colorAdjust > 0.5f) {
        const Transform &cx = env->ColorXfm();
        const float *m0 = &cx.m.x.x;
        const float *m1 = &cx.m.y.x;
        const float *m2 = &cx.m.z.x;
        const float t[3] = {cx.v.x, cx.v.y, cx.v.z};
        for (int c = 0; c < 3; c++) {
            s.colorXfm[c][0] = m0[c];
            s.colorXfm[c][1] = m1[c];
            s.colorXfm[c][2] = m2[c];
            s.colorXfm[c][3] = t[c];
        }
    }
}

// The per-mesh box map, as retail's mesh draw computes it: DxMesh::DrawShowing
// (band.exe TU5 fn_82738E38+0x58) calls fn_82418DC0, which
//  * returns unless RndMesh::sUpdateApproxLight is set (+0x20, the byte at
//    0x82C6FC24), the draw mode is 0 (+0x2c..+0x3c) and there is a current
//    environ (+0x40..+0x4c);
//  * takes the mesh's world sphere centre when MakeWorldSphere succeeds with
//    a radius > 0, else its world origin (+0x50..+0xb0);
//  * calls the environ's UpdateApproxLighting (vtable +0x58) there (+0xb4..
//    +0xc8).
// Character::DrawLodOrShadow clears sUpdateApproxLight and evaluates once at
// the character's sphere centre (rb3-xenon char/Character.cpp, which runs
// natively). Both land in c80..c85 through the decomp's own
// NgEnviron::UpdateApproxLighting and BoxMapLighting, and WgpuShaderMgr keeps
// the registers it was handed, so the draw reads them back as they stand.
// Native's NgEnviron::Select (the HX_NATIVE body) does not evaluate the box
// or count real lights, so the "a real light raises the approx count to 1"
// clause is not reached; the shader's retailLightsOn() also tests points.
template <class E, class M>
void FillRB3XenonApprox(E *env, M *mesh, float box[6][4], float retail[4]) {
    // Mesh::Draw: TheShaderMgr.SetMeshInfo(boneCount, HasAOCalc()).
    retail[0] = mesh->HasAOCalc() ? 1.0f : 0.0f;
    if (!env) return;
    if (M::sUpdateApproxLight && TheRnd.DrawMode() == 0) {
        Sphere sphere;
        Vector3 pos = mesh->WorldXfm().v;
        if (mesh->MakeWorldSphere(sphere, false) && sphere.GetRadius() > 0.0f) pos = sphere.center;
        env->UpdateApproxLighting(&pos);
    }
    retail[1] = (float)env->NumLights_Approx();
    for (int i = 0; i < 6; i++) {
        const float *v = rndshape::CapturedVConstant(0x50 + i);
        box[i][0] = v[0];
        box[i][1] = v[1];
        box[i][2] = v[2];
        box[i][3] = 1.0f;
    }
}

} // namespace

namespace rndshape {

bool WriteSceneLighting(SceneUniforms &s, RndCam *cam) {
    if constexpr (kDC3RetailModel) {
        WriteDC3Retail(s, static_cast<typename DependentEnvT<SceneUniforms>::type *>(
                              RndEnviron::Current()), cam);
        return true;
    } else if constexpr (kRB3RetailLight) {
        // The heuristic arm's own condition: an environ with its ambient/fog
        // owner. Anything else keeps WriteSceneUniforms' default (no environ).
        auto *env = static_cast<typename DependentEnvT<SceneUniforms>::type *>(
            RndEnviron::Current());
        (void)cam;
        if (!env || !env->AmbientFogOwner()) return false;
        WriteRB3XenonRetail(s, env);
        return true;
    } else {
        (void)s;
        return false;
    }
}

void FillMeshApproxLighting(RndMesh *mesh, float box[6][4], float retail[4]) {
    if constexpr (kDC3RetailModel) {
        if (!mesh) return;
        FillDC3Approx(static_cast<typename DependentEnvT<RndMesh>::type *>(RndEnviron::Current()),
                      mesh, box, retail);
    } else if constexpr (kRB3RetailLight) {
        if (!mesh) return;
        auto *env = static_cast<typename DependentEnvT<RndMesh>::type *>(RndEnviron::Current());
        // Only a frame WriteSceneLighting lit with the retail model reads it.
        if (env && !env->AmbientFogOwner()) env = nullptr;
        FillRB3XenonApprox(env, mesh, box, retail);
    } else {
        (void)mesh;
        (void)box;
        (void)retail;
    }
}

RndTex *SceneProjTexture() {
    if constexpr (kDC3RetailModel) {
        return sFrameGobo;
    } else {
        return nullptr;
    }
}

int CollectProjShadowCasterSets(std::vector<ProjShadowCasters> &out) {
    out.clear();
    if constexpr (kDC3RetailModel) {
        sProj.mapCount = 0;
        for (int i = 0; i < sProj.frameCount; i++) {
            ProjShadowCasters set;
            memcpy(set.clipFromWorld, sProj.frameClips[i], sizeof(set.clipFromWorld));
            set.tile = i;
            CollectDC3Casters(set, sProj.frameLights[i],
                              sProj.frameHasFrustum[i] ? &sProj.frameFrustums[i] : nullptr);
            out.push_back(std::move(set));
            sProj.mapLights[i] = sProj.frameLights[i];
            sProj.frameLights[i] = nullptr;
        }
        sProj.mapCount = sProj.frameCount;
        sProj.frameCount = 0;
    }
    return (int)out.size();
}

bool CollectProjShadowCasters(ProjShadowCasters &out) {
    std::vector<ProjShadowCasters> sets;
    if (CollectProjShadowCasterSets(sets) == 0) return false;
    out = std::move(sets.front());
    return true;
}

} // namespace rndshape
