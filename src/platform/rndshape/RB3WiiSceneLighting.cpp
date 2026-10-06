// RB3WiiSceneLighting.cpp — scene lighting for RB3-Wii content under the dc3
// backend (MILO_ENGINE_GPU_BACKEND=dc3, MILO_ENGINE_RNDOBJ_SHAPE=rb3wii only).
//
// WgpuRnd::WriteSceneUniforms was tuned for DC3 venues: every camera lights
// from the current environ, point lights are folded into fake directionals
// aimed at the stage, a fill light is added below three lights, and the
// floor on ambient is 0.08. RB3 content is authored for its own light model,
// and under those DC3 rules the title-screen city and every venue backdrop
// render far too bright.
//
//  * world.cam with an environ (the default): RB3's Xbox 360 retail model,
//    WriteRetailLighting and FillMeshApproxLighting below (doc section 9).
//  * RB3_VENUE_LIGHT_LEGACY=1: the heuristic rb3's own backend (BandRnd,
//    Rnd_Wgpu_RB3.cpp WriteSceneUniforms) converged on, kept under BandRnd's
//    environment-variable names: a near-white ambient scaled by 0.09, all
//    channels floored at 0.008 (re-expressed for gamma-space shading, see
//    ShadingSpace), approx directionals at exposure 0.80, real point lights
//    at 0.70 with the GX inverse-linear falloff, character environs keyed by
//    their real light, and a dim grey key for an environ with no usable light.
//  * any other camera (Cam.cam painting the cloud target, menu and UI cams):
//    a flat 1.0 white key plus 0.45 ambient, as BandRnd does.
//  * no fog: no shipping RB3 environ enables it (RB3_ENV_FOG).
//
// The DC3 shape's WriteSceneLighting is an inline `return false` and its
// FillMeshApproxLighting a no-op, so DC3 and rb3-xenon never reach this code.

#include "platform/rndshape/RndShape.h"
#include "gfx/UniformStructs.h"
#include "char/Character.h"
#include "rndobj/BoxMap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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
// The standard shader shades RB3 in gamma space (rndshape::kGammaSpaceShading):
// it decodes the lit term before multiplying it into the decoded texture, so a
// lighting value v now darkens a texel exactly as much as retail's v does. The
// four ambient fallbacks below were fitted under the old linear shading, where
// the output encode lifted a lit term v to about linearToSrgb(v); they are
// re-expressed through that curve so they keep the brightness they were fitted
// to. They are still read in BandRnd's units, so a value tuned on one backend
// means the same on the other. Light exposures and the grey key are not
// re-expressed: authored light colours are used as authored, and re-expressing
// those two moved the title frame further from retail (doc section 8).
float ShadingSpace(float v) {
    if (!rndshape::kGammaSpaceShading) return v;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float AmbientFloor()       { static float v = ShadingSpace(EnvFloat("RB3_VENUE_AMBIENT_FLOOR", 0.008f)); return v; }
float AmbientClamp()       { static float v = ShadingSpace(EnvFloat("RB3_VENUE_AMBIENT_CLAMP", 0.09f)); return v; }
float GreyKey()            { static float v = EnvFloat("RB3_VENUE_GREY_KEY", 0.22f); return v; }
float CharApproxAmbient()  { static float v = ShadingSpace(EnvFloat("RB3_CHAR_APPROX_AMBIENT", 0.11f)); return v; }
float CharAmbientMax()     { static float v = ShadingSpace(EnvFloat("RB3_CHAR_AMBIENT_MAX", 0.14f)); return v; }
float PointExposure()      { static float v = EnvFloat("RB3_VENUE_POINT_EXPOSURE", 0.70f); return v; }
float DirExposure()        { static float v = EnvFloat("RB3_VENUE_DIR_EXPOSURE", 0.80f); return v; }

bool Usable(RndLight* L) { return L && L->mColorOwner && L->Showing(); }

void SetDirLight(SceneUniforms& s, int i, float x, float y, float z, float r, float g, float b) {
    s.lightDirs[i][0] = x; s.lightDirs[i][1] = y; s.lightDirs[i][2] = z; s.lightDirs[i][3] = 0.0f;
    s.lightColors[i][0] = r; s.lightColors[i][1] = g; s.lightColors[i][2] = b; s.lightColors[i][3] = 1.0f;
}


bool RetailLightModel() { static int v = EnvFlag("RB3_VENUE_LIGHT_LEGACY") ? 0 : 1; return v != 0; }

bool IsWorldCam(RndCam* cam) {
    const char* camName = cam ? cam->Name() : nullptr;
    // WorldReflection::DrawShowing draws the mirrored venue through an unnamed
    // deep copy of world.cam in draw mode 7; it is the venue camera too.
    return (camName && std::strcmp(camName, "world.cam") == 0) ||
           TheRnd->DrawMode() == rndshape::kDrawModeReflection;
}

// Whether the retail model lights draws made now (WriteSceneLighting's test,
// repeated per mesh by FillMeshApproxLighting).
bool RetailLightingActive(RndCam* cam, RndEnviron* env) {
    return VenueLightEnabled() && RetailLightModel() && IsWorldCam(cam) && env &&
           env->mAmbientFogOwner;
}

// Retail's per-draw "is this light on" test: Showing and a non-zero packed
// colour (NgEnviron CheckPointLight / SetPointLightRegisters).
bool RetailLit(RndLight* L) { return L && L->mColorOwner && L->Showing() && L->GetColor().Pack() != 0; }

// RB3's own light model, as the Xbox 360 retail build runs it. Sources:
// rb3-xenon rndobj/Env_NG.cpp (NgEnviron::Select, UpdateApproxLighting),
// rndobj/BoxMap.cpp, rndobj/Mat_NG.cpp (NgMat::SetupAmbient), rndobj/Shader.cpp
// (option bits), char/Character.cpp (DrawLodOrShadow), rndobj/Mesh.cpp
// (sUpdateApproxLight), and the shipped xbox_shaders `standard` permutations
// (doc section 9).
//
//  * ambient: the environ's ambient colour, unscaled (constant c1);
//  * real lights: at most two point lights (c64/c67 and c65/c68), Lambert,
//    full strength inside falloffStart and linear to zero at range; at most
//    one projected light. Written here, once per environ;
//  * approx lights (mLightsApprox, plus the spotlight set in
//    RndEnviron::sGlobalLighting): folded on the CPU into a six-face box map
//    (c80..c85) at the mesh's world sphere centre, or the character's for a
//    mesh inside a Character. Directional lights always land there; a real
//    list holds only point and projected lights (IsValidRealLight). Written
//    per draw by FillMeshApproxLighting;
//  * standard.vs then computes material * (ambient + box(N) + points) per
//    vertex, and standard.ps multiplies by the texel, with no clamp until the
//    8-bit target.
//
// Projected lights are not uploaded. Every one the shipped venues carry is
// blend 1 (`shadow_projected.lit`), which retail uses only to darken the light
// terms inside the projected shadow map, in the per_pixel_lit permutations
// alone; it adds no light. Fog stays off (no shipped RB3 environ enables it;
// RB3_ENV_FOG).
void WriteRetailLighting(SceneUniforms& s, RndEnviron* env) {
    s.retailLighting = 1.0f;
    s.pointFalloffMode = 2.0f;
    s.venueHighlightLumaMode = 0.0f;
    s.numLights = 0.0f;

    const Hmx::Color& amb = env->AmbientColor();
    s.ambientColor[0] = amb.red;
    s.ambientColor[1] = amb.green;
    s.ambientColor[2] = amb.blue;
    s.ambientColor[3] = 1.0f;

    // Real lights: retail keeps the first two lit point lights, in list order.
    int pl = 0;
    for (ObjPtrList<RndLight>::iterator it = env->mLightsReal.begin();
         it != env->mLightsReal.end() && pl < 2; ++it) {
        RndLight* L = *it;
        if (!RetailLit(L) || L->GetType() != RndLight::kPoint) continue;
        const Vector3& p = L->WorldXfm().v;
        const Hmx::Color& c = L->GetColor();
        float slope = 0.0f, rangeScale = 1.0f;
        if (L->FalloffStart() < L->Range()) {
            slope = 1.0f / (L->FalloffStart() - L->Range());
            rangeScale = -(L->Range() * slope);
        }
        s.pointLightPos[pl][0] = p.x; s.pointLightPos[pl][1] = p.y;
        s.pointLightPos[pl][2] = p.z; s.pointLightPos[pl][3] = slope;
        s.pointLightColors[pl][0] = c.red; s.pointLightColors[pl][1] = c.green;
        s.pointLightColors[pl][2] = c.blue; s.pointLightColors[pl][3] = rangeScale;
        s.pointLightRanges[pl] = L->Range();
        pl++;
    }
    s.numPointLights = (float)pl;
}

// The position retail evaluates a mesh's approx lights at. RndMesh draws
// recompute the box map at their own world sphere centre (falling back to the
// mesh origin) while RndMesh::sUpdateApproxLight is set, which is its default;
// Character::DrawLodOrShadow clears it around its meshes and evaluates once at
// the character's world sphere centre instead. The character's sphere is
// cached per frame.
Vector3 ApproxLightPos(RndMesh* mesh) {
    for (RndTransformable* t = mesh->TransParent(); t; t = t->TransParent()) {
        Character* ch = dynamic_cast<Character*>(t);
        if (!ch) continue;
        struct Cached { int frame; bool ok; Vector3 center; };
        static std::unordered_map<Character*, Cached> sCache;
        const int frame = TheRnd->GetFrameID();
        Cached& c = sCache[ch];
        if (c.frame != frame || frame == 0) {
            Sphere sp;
            c.ok = ch->MakeWorldSphere(sp, false) && sp.GetRadius() > 0;
            c.center = sp.center;
            c.frame = frame;
        }
        if (c.ok) return c.center;
        break;
    }
    Sphere sp;
    if (mesh->MakeWorldSphere(sp, false) && sp.GetRadius() > 0) return sp.center;
    return mesh->WorldXfm().v;
}

} // namespace

namespace rndshape {

void FillMeshApproxLighting(RndMesh* mesh, float box[6][4]) {
    RndEnviron* env = RndEnviron::sCurrent;
    if (!mesh || !RetailLightingActive(RndCam::sCurrent, env)) return;
    // NgEnviron::UpdateApproxLighting.
    if (!(env->UsesApproxLocal() || env->UsesApproxGlobal())) return;
    if (env->mLightsReal.empty() && env->mLightsApprox.empty()) return;
    const Vector3 pos = ApproxLightPos(mesh);
    Hmx::Color faces[6];
    for (int i = 0; i < 6; i++) faces[i].Set(0, 0, 0);
    if (env->UsesApproxLocal()) {
        static BoxMapLighting sBoxLight;
        sBoxLight.Clear();
        for (ObjPtrList<RndLight>::iterator it = env->mLightsApprox.begin();
             it != env->mLightsApprox.end(); ++it) {
            if (*it) sBoxLight.QueueLight(*it, 1.0f);
        }
        sBoxLight.ApplyQueuedLights(faces, &pos);
    }
    if (env->UsesApproxGlobal() && RndEnviron::sGlobalLighting.NumQueuedLights() != 0)
        RndEnviron::sGlobalLighting.ApplyQueuedLights(faces, &pos);
    for (int i = 0; i < 6; i++) {
        box[i][0] = faces[i].red;
        box[i][1] = faces[i].green;
        box[i][2] = faces[i].blue;
        box[i][3] = 1.0f;
    }
}

bool WriteSceneLighting(SceneUniforms& s, RndCam* cam) {
    const bool worldCam = IsWorldCam(cam);
    RndEnviron* env = RndEnviron::sCurrent;

    if (RetailLightingActive(cam, env)) {
        WriteRetailLighting(s, env);
        return true;
    }
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
