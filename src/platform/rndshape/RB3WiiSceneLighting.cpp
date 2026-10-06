// RB3WiiSceneLighting.cpp — scene lighting for RB3-Wii content under the dc3
// backend (MILO_ENGINE_GPU_BACKEND=dc3, MILO_ENGINE_RNDOBJ_SHAPE=rb3wii only).
//
// WgpuRnd::WriteSceneUniforms was tuned for DC3 venues: every camera lights
// from the current environ, point lights are folded into fake directionals
// aimed at the stage, a fill light is added below three lights, and the
// floor on ambient is 0.08. RB3 content authors its lighting for the Wii's
// GX pipeline instead, and under those DC3 rules the title-screen city and
// every venue backdrop render far too bright (title frame 400: city mean
// luma 68.7 vs retail 52.4).
//
// This file is the lighting model rb3's own backend (BandRnd,
// Rnd_Wgpu_RB3.cpp WriteSceneUniforms) has converged on, moved behind the
// rndshape seam so the dc3 backend renders RB3 the same way. The tunables
// keep BandRnd's environment-variable names and defaults so a tuning found on
// one backend applies to the other.
//
//  * world.cam with a usable environ: ambient taken from the environ, a
//    near-white ambient (the engine's unauthored default) scaled by 0.09, all
//    channels floored at 0.008. Directional lights from the approx list (dir
//    exposure 0.80, capped 1.5); point lights stay real point lights (point
//    exposure 0.70, capped 1.8) with the GX inverse-linear falloff. Character
//    environs (name contains "char") that carry a real key shade from the
//    real list and fold the approx set into ambient. An environ with no
//    usable light gets a dim grey key.
//  * any other camera (Cam.cam painting the cloud target, menu and UI cams):
//    a flat 1.0 white key plus 0.45 ambient, as BandRnd does.
//  * no fog and no projected light: BandRnd ships both off (RB3_ENV_FOG,
//    RB3_ENV_PROJLIGHT) and no shipping RB3 environ enables fog.
//
// The DC3 shape's WriteSceneLighting is an inline `return false`, so DC3 and
// rb3-xenon never reach this code.

#include "platform/rndshape/RndShape.h"
#include "gfx/UniformStructs.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

float EnvFloat(const char* name, float def) {
    const char* e = getenv(name);
    if (!e || !e[0]) return def;
    float v = (float)atof(e);
    return v >= 0.f ? v : def;
}

bool EnvFlag(const char* name) {
    const char* e = getenv(name);
    return e && e[0] && e[0] != '0';
}

bool VenueLightEnabled()   { static int v = EnvFlag("RB3_VENUE_LIGHT_OFF") ? 0 : 1; return v != 0; }
bool PointFalloffGx()      { static int v = EnvFlag("RB3_VENUE_POINT_FALLOFF_LEGACY") ? 0 : 1; return v != 0; }
bool WhiteGuard()          { static int v = EnvFlag("RB3_VENUE_WHITE_GUARD") ? 1 : 0; return v != 0; }
bool CharRealLight()       { static int v = EnvFlag("RB3_CHAR_REAL_LIGHT_OFF") ? 0 : 1; return v != 0; }
bool FallbackFix()         { static int v = EnvFlag("RB3_VENUE_FALLBACK_FIX") ? 1 : 0; return v != 0; }
float AmbientFloor()       { static float v = EnvFloat("RB3_VENUE_AMBIENT_FLOOR", 0.008f); return v; }
float AmbientClamp()       { static float v = EnvFloat("RB3_VENUE_AMBIENT_CLAMP", 0.09f); return v; }
float GreyKey()            { static float v = EnvFloat("RB3_VENUE_GREY_KEY", 0.22f); return v; }
float CharApproxAmbient()  { static float v = EnvFloat("RB3_CHAR_APPROX_AMBIENT", 0.11f); return v; }
float CharAmbientMax()     { static float v = EnvFloat("RB3_CHAR_AMBIENT_MAX", 0.14f); return v; }
float PointExposure()      { static float v = EnvFloat("RB3_VENUE_POINT_EXPOSURE", 0.70f); return v; }
float DirExposure()        { static float v = EnvFloat("RB3_VENUE_DIR_EXPOSURE", 0.80f); return v; }

bool Usable(RndLight* L) { return L && L->mColorOwner && L->Showing(); }

void SetDirLight(SceneUniforms& s, int i, float x, float y, float z, float r, float g, float b) {
    s.lightDirs[i][0] = x; s.lightDirs[i][1] = y; s.lightDirs[i][2] = z; s.lightDirs[i][3] = 0.0f;
    s.lightColors[i][0] = r; s.lightColors[i][1] = g; s.lightColors[i][2] = b; s.lightColors[i][3] = 1.0f;
}

} // namespace

namespace rndshape {

bool WriteSceneLighting(SceneUniforms& s, RndCam* cam) {
    const char* camName = cam ? cam->Name() : nullptr;
    // WorldReflection::DrawShowing draws the mirrored venue through an unnamed
    // deep copy of world.cam in draw mode 7; it is the venue camera too.
    const bool worldCam = (camName && std::strcmp(camName, "world.cam") == 0) ||
                          TheRnd->DrawMode() == kDrawModeReflection;
    RndEnviron* env = RndEnviron::sCurrent;

    if (VenueLightEnabled() && worldCam && env && env->mAmbientFogOwner) {
        s.pointFalloffMode = PointFalloffGx() ? 1.0f : 0.0f;
        s.venueHighlightLumaMode = WhiteGuard() ? 1.0f : 0.0f;

        const Hmx::Color& amb = env->AmbientColor();
        float ar = amb.red, ag = amb.green, ab = amb.blue;
        if (std::max(ar, std::max(ag, ab)) > 0.85f) {
            const float k = AmbientClamp();
            ar *= k; ag *= k; ab *= k;
        }
        const float floor = AmbientFloor();
        s.ambientColor[0] = std::max(ar, floor);
        s.ambientColor[1] = std::max(ag, floor);
        s.ambientColor[2] = std::max(ab, floor);
        s.ambientColor[3] = 1.0f;

        const char* envName = env->Name() ? env->Name() : "";
        bool useReal = false;
        if (CharRealLight() && std::strstr(envName, "char")) {
            for (ObjPtrList<RndLight>::iterator it = env->mLightsReal.begin();
                 it != env->mLightsReal.end(); ++it) {
                if (!Usable(*it)) continue;
                const Hmx::Color& c = (*it)->GetColor();
                if (c.red + c.green + c.blue > 0.01f) { useReal = true; break; }
            }
        }

        ObjPtrList<RndLight>& lights = useReal ? env->mLightsReal : env->mLightsApprox;
        int dl = 0, pl = 0;
        for (ObjPtrList<RndLight>::iterator it = lights.begin();
             it != lights.end() && (dl < 4 || pl < 4); ++it) {
            RndLight* L = *it;
            if (!Usable(L)) continue;
            const Hmx::Color& c = L->GetColor();
            if (c.red + c.green + c.blue <= 0.01f) continue;
            if (L->GetType() == RndLight::kDirectional && dl < 4) {
                const Vector3& d = L->WorldXfm().m.y;
                const float e = DirExposure();
                SetDirLight(s, dl++, d.x, d.y, d.z,
                            std::min(c.red * e, 1.5f), std::min(c.green * e, 1.5f),
                            std::min(c.blue * e, 1.5f));
            } else if (L->GetType() == RndLight::kPoint && pl < 4) {
                const Vector3& p = L->WorldXfm().v;
                const float e = PointExposure();
                s.pointLightPos[pl][0] = p.x; s.pointLightPos[pl][1] = p.y;
                s.pointLightPos[pl][2] = p.z; s.pointLightPos[pl][3] = 0.0f;
                s.pointLightColors[pl][0] = std::min(c.red * e, 1.8f);
                s.pointLightColors[pl][1] = std::min(c.green * e, 1.8f);
                s.pointLightColors[pl][2] = std::min(c.blue * e, 1.8f);
                s.pointLightColors[pl][3] = 1.0f;
                s.pointLightRanges[pl] = L->Range() > 0.f ? L->Range() : 100.f;
                pl++;
            }
        }

        if (useReal) {
            // Fold the character environ's approx set (rim + silhouette spots)
            // into ambient as an average, clamped low so the real key carries
            // the directional shading.
            float fr = 0, fg = 0, fb = 0;
            int n = 0;
            for (ObjPtrList<RndLight>::iterator it = env->mLightsApprox.begin();
                 it != env->mLightsApprox.end(); ++it) {
                if (!Usable(*it)) continue;
                const Hmx::Color& c = (*it)->GetColor();
                if (c.red + c.green + c.blue <= 0.01f) continue;
                fr += c.red; fg += c.green; fb += c.blue; n++;
            }
            if (n > 0) {
                const float k = CharApproxAmbient() / (float)n;
                const float mx = CharAmbientMax();
                s.ambientColor[0] = std::min(s.ambientColor[0] + fr * k, mx);
                s.ambientColor[1] = std::min(s.ambientColor[1] + fg * k, mx);
                s.ambientColor[2] = std::min(s.ambientColor[2] + fb * k, mx);
            }
        }

        if (dl == 0 && pl == 0) {
            // Ambient-only environ (sky, road groups): a dim grey key so the
            // geometry keeps its form without washing out authored colour.
            const float g = GreyKey() * DirExposure();
            SetDirLight(s, 0, -0.4f, -0.5f, -0.75f, g, g, g);
            dl = 1;
        }
        s.numLights = (float)dl;
        s.numPointLights = (float)pl;
        return true;
    }

    // Every other camera, and a world.cam frame whose environ is unusable.
    s.numPointLights = 0.0f;
    s.numLights = 1.0f;
    if (FallbackFix() && worldCam) {
        SetDirLight(s, 0, -0.4f, -0.5f, -0.75f, 0.5f, 0.5f, 0.5f);
        s.ambientColor[0] = s.ambientColor[1] = s.ambientColor[2] = 0.10f;
    } else {
        SetDirLight(s, 0, -0.4f, -0.5f, -0.75f, 1.0f, 1.0f, 1.0f);
        s.ambientColor[0] = s.ambientColor[1] = s.ambientColor[2] = 0.45f;
    }
    s.ambientColor[3] = 1.0f;
    return true;
}

} // namespace rndshape
