// DC3SceneLighting.cpp — DC3's own light model behind the rndshape seam
// (MILO_ENGINE_GPU_BACKEND=dc3, MILO_ENGINE_RNDOBJ_SHAPE=dc3). Built for both
// consumers of the DC3 shape; every body below is selected by
// rndshape::kDC3RetailModel (RndShape_DC3.h), which is true for dc3-decomp and
// false for rb3-xenon, so rb3-xenon gets `return false` / no-ops and keeps
// WgpuRnd::WriteSceneUniforms' own environ block exactly as before.
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
//    at range; at most one projected light in slot 3 (not drawn here yet);
//  * approx lights: NgEnviron::UpdateApproxLighting folds mLightsApprox and
//    RndEnviron::sGlobalLighting into the six-face box map c80..c85 at the
//    position Select was given -- the character's origin for a Character
//    (Character::DrawLodOrShadow's RndEnvironTracker), the RndDir's for an
//    RndDir, none for a WorldDir (then points and spots in the approx set are
//    skipped and only directionals and fake spots land). The native port runs
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

#include "obj/Dir.h"
#include "rndobj/Group.h"
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
struct ProjShadowState {
    RndLight *frameLight = nullptr;   // this frame's light; never kept across frames
    float frameClip[16] = {};
    const void *mapLight = nullptr;   // whose casters the map holds (identity only)
    bool mapValid = false;
};
ProjShadowState sProj;

// RndLight::mShadowObjects is protected and has no accessor; a member pointer
// named through a derived class reaches it without touching the decomp.
template <class L>
struct LightShadowObjects : L {
    static auto Member() { return &LightShadowObjects::mShadowObjects; }
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

template <class E>
void WriteDC3ProjShadow(SceneUniforms &s, E *env) {
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
    // 2: the map is last frame's draw of this light's casters.
    s.shadowEnabled = (sProj.mapValid && sProj.mapLight == proj) ? 2.0f : 0.0f;
    if (!sProj.frameLight) {
        sProj.frameLight = proj;
        memcpy(sProj.frameClip, clip, sizeof(clip));
    }
}

// Every mesh a caster draws. A Character / RndDir is an ObjectDir; a group
// draws its members. Templated so the names are checked only for dc3-decomp.
template <class Mesh, class Group, class Dir>
void AddCasterMeshes(Hmx::Object *o, std::vector<RndMesh *> &out, int depth) {
    if (!o || depth > 8) return;
    if (Mesh *m = dynamic_cast<Mesh *>(o)) {
        if (m->Showing()) out.push_back(m);
        return;
    }
    if (RndDrawable *d = dynamic_cast<RndDrawable *>(o)) {
        if (!d->Showing()) return;
    }
    if (Group *g = dynamic_cast<Group *>(o)) {
        for (auto it = g->Objects().begin(); it != g->Objects().end(); ++it)
            AddCasterMeshes<Mesh, Group, Dir>(*it, out, depth + 1);
        return;
    }
    if (Dir *dir = dynamic_cast<Dir *>(o)) {
        for (ObjDirItr<Mesh> m(dir, true); m; ++m) {
            if (m->Showing()) out.push_back(m);
        }
    }
}

template <class L>
bool CollectDC3Casters(rndshape::ProjShadowCasters &out, L *light) {
    auto *list = light->GetShadowOverride();
    if (!list || list->size() == 0) list = &(light->*LightShadowObjects<L>::Member());
    for (auto it = list->begin(); it != list->end(); ++it)
        AddCasterMeshes<RndMesh, RndGroup, ObjectDir>(*it, out.meshes, 0);
    return true;
}

template <class E>
void WriteDC3Retail(SceneUniforms &s, E *env) {
    s.retailLighting = 2.0f;      // 2: DC3's form of the retail model
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

    WriteDC3ProjShadow(s, env);

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

} // namespace

namespace rndshape {

bool WriteSceneLighting(SceneUniforms &s, RndCam *) {
    if constexpr (kDC3RetailModel) {
        WriteDC3Retail(s, static_cast<typename DependentEnvT<SceneUniforms>::type *>(
                              RndEnviron::Current()));
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

bool CollectProjShadowCasters(ProjShadowCasters &out) {
    if constexpr (kDC3RetailModel) {
        RndLight *light = sProj.frameLight;
        sProj.frameLight = nullptr;
        if (!light) {
            sProj.mapValid = false;
            sProj.mapLight = nullptr;
            return false;
        }
        memcpy(out.clipFromWorld, sProj.frameClip, sizeof(sProj.frameClip));
        CollectDC3Casters(out, light);
        sProj.mapValid = true;
        sProj.mapLight = light;
        return true;
    } else {
        (void)out;
        return false;
    }
}

} // namespace rndshape
