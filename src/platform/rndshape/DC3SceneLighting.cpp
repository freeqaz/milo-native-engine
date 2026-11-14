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
#include "obj/Data.h"
#include "os/System.h"

#include <cstring>

namespace {

template <class E>
void WriteDC3Retail(SceneUniforms &s, E *env) {
    s.retailLighting = 2.0f;      // 2: DC3's form of the retail model
    s.pointFalloffMode = 2.0f;
    s.venueHighlightLumaMode = 0.0f;
    s.numLights = 0.0f;
    s.numPointLights = 0.0f;
    s.numProjLights = 0.0f;
    s.aoStrength = 0.0f;
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

template <class E>
void FillDC3Approx(E *env, float box[6][4], float retail[4]) {
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
                      box, retail);
    } else {
        (void)mesh;
        (void)box;
        (void)retail;
    }
}

float DisplayGamma(bool presenting) {
    if constexpr (kDC3RetailModel) {
        if (!presenting) return 0.0f;
        // DxRnd::SetupGamma: SystemConfig("rnd")->FindData("gamma", g, false).
        // Not cached until the system config exists.
        static float sGamma = -1.0f;
        if (sGamma >= 0.0f) return sGamma;
        DataArray *root = SystemConfig();
        if (!root) return 0.0f;
        DataArray *rnd = root->FindArray("rnd", false);
        float g = 0.0f;
        sGamma = (rnd && rnd->FindData("gamma", g, false) && g > 0.0f) ? g : 0.0f;
        return sGamma;
    } else {
        (void)presenting;
        return 0.0f;
    }
}

} // namespace rndshape
