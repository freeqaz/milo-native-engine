// DC3PostChain.cpp — np-gfx-post: fills gfx/RB3RetailPost's DC3 parameters
// from a dc3-decomp RndPostProc the way NgPostProc::DoPost sets the retail
// shader constants (dc3-decomp rndobj/PostProc_NG.cpp: DoBloom, SetBloomColor,
// ModulateColorXfm, CheckNoise, CheckChromaticAberration, CheckVignette,
// CheckMotionBlur), and
// the display ramp (DxRnd::SetupGamma). Built for the DC3 shape on the dc3
// backend, i.e. for dc3-decomp AND rb3-xenon; everything here is gated on
// rndshape::kDC3RetailPost, which only dc3-decomp's accessors turn on, so
// rb3-xenon's mode is 0 and its output is unchanged.
#include "gfx/DisplayRamp.h"
#include "platform/rndshape/RndShape.h"
#include "gfx/RB3RetailPost.h"
#include "platform/TexGpu.h"

#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"
#include "rndobj/HiResScreen.h"
#include "rndobj/Rnd.h"
#if __has_include("rndobj/VelocityBuffer.h")
#include "rndobj/VelocityBuffer.h"
#endif
#include "obj/Data.h"
#include "os/System.h"
#include "os/Debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

// rndshape::DofSetCamPlanes's source in the DC3 shape. dc3-decomp's
// NativeDOFProc (src/system/rndobj/DOFProc.cpp) defines the strong symbol;
// rb3-xenon defines none and gets this answer: no Set camera, so DofPass reads
// the current camera as it always has.
__attribute__((weak)) bool NativeDOFSetCamPlanes(float &, float &, float &, float &) {
    return false;
}

namespace rndshape {

namespace {
template <class R, class = void> struct RndHasBlacklightOverride : std::false_type {};
template <class R>
struct RndHasBlacklightOverride<
    R, std::void_t<decltype(std::declval<const R &>().GetPostProcBlacklightOverride()),
                   decltype(static_cast<RndPostProc *>(std::declval<R &>().GetPostProcOverride()))>>
    : std::true_type {};
template <class R> RndPostProc *ActiveOf(R &rnd) {
    if constexpr (RndHasBlacklightOverride<R>::value) {
        if (RndPostProc *b = rnd.GetPostProcBlacklightOverride()) return b;
        if (RndPostProc *o = rnd.GetPostProcOverride()) return o;
    } else {
        (void)rnd;
    }
    return RndPostProc::Current();
}
}  // namespace

RndPostProc *ActivePostProc() { return ActiveOf(TheRnd); }

namespace {
template <class R, class = void> struct RndHasDisablePP : std::false_type {};
template <class R>
struct RndHasDisablePP<R, std::void_t<decltype(std::declval<const R &>().DisablePP())>>
    : std::true_type {};
template <class R> bool DisabledOf(const R &rnd) {
    if constexpr (RndHasDisablePP<R>::value) {
        return rnd.DisablePP();
    } else {
        (void)rnd;
        return false;
    }
}

#if __has_include("rndobj/VelocityBuffer.h")
template <class V, class = void> struct HasTakeNativeFrame : std::false_type {};
template <class V>
struct HasTakeNativeFrame<
    V, std::void_t<decltype(std::declval<V &>().TakeNativeFrame(std::declval<bool &>(),
                                                                std::declval<float &>(),
                                                                std::declval<RndCam *&>()))>>
    : std::true_type {};
// The singleton is named only where the accessor exists, so a consumer whose
// VelocityBuffer.cpp is not linked (rb3-xenon) never references it.
template <class V> bool TakeOf(bool &ready, float &scale, RndCam *&cam) {
    if constexpr (HasTakeNativeFrame<V>::value) {
        return V::Singleton().TakeNativeFrame(ready, scale, cam);
    } else {
        (void)ready;
        (void)scale;
        (void)cam;
        return false;
    }
}
#endif
}  // namespace

bool PostProcDisabled() { return DisabledOf(TheRnd); }

bool TakeVelocityFrame(bool &ready, float &scale, RndCam *&cam) {
#if __has_include("rndobj/VelocityBuffer.h")
    return TakeOf<RndVelocityBuffer>(ready, scale, cam);
#else
    (void)ready;
    (void)scale;
    (void)cam;
    return false;
#endif
}

int RetailPostMode() {
    if constexpr (kRetailPostChain) {
        // lane A55: RB3's own chain (rb3-xenon), with the RB3-Wii shape's
        // knob and grammar (rndshape/RB3WiiPostChain.cpp).
        static int s = -1;
        if (s < 0) {
            const char *e = getenv("MILO_RB3_RETAIL_POST");
            s = !e ? 1 : (e[0] == '0') ? 0 : (strcmp(e, "raw") == 0) ? 2
                : (strcmp(e, "mask") == 0) ? 3 : (strcmp(e, "bloom") == 0) ? 4
                : (strcmp(e, "grade") == 0) ? 5 : (strcmp(e, "beams") == 0) ? 6 : 1;
            fprintf(stderr, "RB3 post: retail X360 chain mode %d (MILO_RB3_RETAIL_POST=%s)\n", s,
                    e ? e : "unset");
        }
        return s;
    } else if constexpr (!kDC3RetailPost) {
        return 0;
    } else {
        static int s = -1;
        if (s < 0) {
            // Values beyond the registry text (NativeCompatFlags.classification.json,
            // kept unchanged so rb3's generated ledger stays current):
            // 'velocity' shows the velocity target (tf10) as stored; 'veldiff' shows
            // |bit 42's blur - scene| x 8 in the same frame. Unset also runs the camera
            // velocity blur wherever the post-proc's motion_blur_velocity is on.
            const char *e = getenv("DC3_RETAIL_POST");
            s = !e ? 1 : (e[0] == '0') ? 0 : (strcmp(e, "raw") == 0) ? 2
                : (strcmp(e, "mask") == 0) ? 3 : (strcmp(e, "bloom") == 0) ? 4
                : (strcmp(e, "grade") == 0) ? 5 : (strcmp(e, "velocity") == 0) ? 6
                : (strcmp(e, "veldiff") == 0) ? 7 : 1;
            MILO_LOG("DC3 post: retail chain mode %d (DC3_RETAIL_POST=%s)\n", s, e ? e : "unset");
        }
        return s;
    }
}

namespace {
// {rnd native_look display_*} (dc3 rndobj/Rnd.cpp NativeLook): -1 = the
// environment's / the default, 0 = no ramp, else a display type.
int gDisplayInspect = -1;

// DC3_DISPLAY_GAMMA, read once. Grammar: unset = the console on a TV (display
// type TV, the system config's gamma); "off" or 0 = no ramp (the pre-ramp
// front buffer); "srgb" = the ramp alone, scanned out as given (a VGA-cabled
// console: type sRGB, the picture before item 31); "tv" = the default
// spelled out; a positive number = that gamma on a TV; "srgb:<g>" / "tv:<g>"
// = both.
struct DisplayEnv {
    bool off = false;
    float gamma = 0.0f;   // 0: the system config's
    int type = DisplayRamp::kDisplayTV;
};
const DisplayEnv &Env() {
    static DisplayEnv env;
    static bool read = false;
    if (!read) {
        read = true;
        if (const char *e = getenv("DC3_DISPLAY_GAMMA")) {
            const char *num = e;
            if (strncmp(e, "srgb", 4) == 0) {
                env.type = DisplayRamp::kDisplaySRGB;
                num = e + 4;
            } else if (strncmp(e, "tv", 2) == 0) {
                num = e + 2;
            }
            if (*num == ':') num++;
            if (strcmp(e, "off") == 0) env.off = true;
            else if (*num) {
                const float v = (float)atof(num);
                if (v > 0.0f) env.gamma = v;
                else env.off = true;
            }
        }
    }
    return env;
}

// DxRnd::SetupGamma: SystemConfig("rnd")->FindData("gamma", gamma, false);
// DC3's config/rnd.dta #merges system/run/config/default.dta, which sets
// (gamma 0.85). Not cached until the system config exists.
float ConfigGamma() {
    static float s = -1.0f;
    if (s >= 0.0f) return s;
    DataArray *root = SystemConfig();
    if (!root) return 0.0f;
    DataArray *rnd = root->FindArray("rnd", false);
    float g = 0.0f;
    s = (rnd && rnd->FindData("gamma", g, false) && g > 0.0f) ? g : 0.0f;
    MILO_LOG("DC3 display ramp: gamma %.3f on every output, display type %s (captures are %s)\n",
             s, Env().type == DisplayRamp::kDisplaySRGB ? "sRGB" : "TV",
             s > 0.0f ? "POST-ramp; DC3_DISPLAY_GAMMA=off for pre-ramp" : "pre-ramp");
    return s;
}
}  // namespace

void SetDisplayTypeForInspection(int type) {
    gDisplayInspect = (type == 0 || type == DisplayRamp::kDisplaySRGB ||
                       type == DisplayRamp::kDisplayTV) ? type : -1;
    // One line per switch, in the form of every other look token's, so a
    // capture tool can find the moment a display token took in the log.
    fprintf(stderr, "[look] display curve %s (inspection)\n",
            gDisplayInspect == 0 ? "off"
            : gDisplayInspect == DisplayRamp::kDisplaySRGB ? "srgb"
            : gDisplayInspect == DisplayRamp::kDisplayTV ? "tv" : "the launch's (the rule)");
}

int DisplayTypeForInspection() { return gDisplayInspect; }

float DisplayGamma(bool presenting) {
    (void)presenting;
    if constexpr (!kDC3RetailPost) {
        return 0.0f;
    } else {
        // Every output, headless included: frames captured natively are what
        // the TV shows (post-ramp).
        if (gDisplayInspect == 0) return 0.0f;
        const DisplayEnv &env = Env();
        if (env.off && gDisplayInspect < 0) return 0.0f;
        return env.gamma > 0.0f ? env.gamma : ConfigGamma();
    }
}

int DisplayGammaType() {
    if constexpr (!kDC3RetailPost) {
        return DisplayRamp::kDisplaySRGB;
    } else {
        return gDisplayInspect > 0 ? gDisplayInspect : Env().type;
    }
}

// NgPostProc::SetBloomColor: c7 = (0.3, 0.59, 0.11) / d, d = threshold when
// it is above 1, else 1.
float BloomLumaScale(const RndPostProc *pp) {
    const float thr = pp->GetBloomThreshold();
    return (1.0f - thr >= 0.0f) ? 1.0f : 1.0f / thr;
}

namespace {
template <class P>
void FillDC3(const P *pp, float flickerMul, RetailPostParams &out) {
    if constexpr (!PostProcHasBloomLocFactor<P>::value) {
        (void)pp;
        (void)flickerMul;
        (void)out;
    } else {
        (void)flickerMul;   // DC3 takes the step modulation the game computed
        out.dc3 = true;
        // DoBloom: doBloom = BloomIntensity() > 0 || bloomColor.alpha > 0;
        // c6 = bloomColor.rgb * sBloomLocFactor * BloomIntensity().
        const float intensity = pp->BloomIntensity();
        const Hmx::Color &bc = pp->GetBloomColor();
        out.bloom = intensity > 0.0f || bc.alpha > 0.0f;
        const float k = P::BloomLocFactor() * intensity;
        out.bloomColor[0] = bc.red * k;
        out.bloomColor[1] = bc.green * k;
        out.bloomColor[2] = bc.blue * k;
        out.bloomLumaScale = BloomLumaScale(pp);
        // The streak arm runs when mBloomStreak and not mBloomGlare; glare
        // takes its own arm (doGlare = mBloomGlare && !TheHiResScreen.
        // IsActive()), the plain one on the hi-res screen.
        out.glare = pp->GetBloomGlare() && !TheHiResScreen.IsActive();
        out.streak = pp->GetBloomStreak() && !pp->GetBloomGlare();
        out.streakAttenuation = pp->GetBloomStreakAttenuation();
        out.streakAngle = pp->GetBloomStreakAngle();
        const float w = (float)TheRnd.Width(), h = (float)TheRnd.Height();
        out.screenAspect = w > 0.0f ? h / w : 0.5625f;

        // ModulateColorXfm: the RndColorXfm matrix with its 3x3 scaled by
        // mColorModulation (RndPostProc::UpdateColorModulation's step, run by
        // the game's own RndPostProc::DoPost). Milo transforms colours as row
        // vectors, so output channel i takes column i of the 3x3.
        const float mod = pp->GetColorModulation();
        const Transform &t = pp->GetColorXfm().mColorXfm;
        const Vector3 *rows[3] = {&t.m.x, &t.m.y, &t.m.z};
        for (int i = 0; i < 3; i++) {
            const float vi = i == 0 ? t.v.x : i == 1 ? t.v.y : t.v.z;
            for (int j = 0; j < 3; j++) {
                const Vector3 &r = *rows[j];
                out.xfm[i][j] = (i == 0 ? r.x : i == 1 ? r.y : r.z) * mod;
            }
            out.xfm[i][3] = vi;
        }

        // CheckVignette (DoVignette: intensity != 0).
        out.vignetteIntensity = pp->GetVignetteIntensity();
        const Hmx::Color &vc = pp->GetVignetteColor();
        out.vignetteColor[0] = vc.red;
        out.vignetteColor[1] = vc.green;
        out.vignetteColor[2] = vc.blue;
        out.vignetteColor[3] = vc.alpha;
        // CheckChromaticAberration (DoChromaticAberration: offset != 0).
        out.chromaticOffset = pp->GetChromaticAberrationOffset();
        out.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
        out.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;
        // CheckMotionBlur: DoMotionBlur() = blend > 0 && weight.Pack() > 0 &&
        // !TheHiResScreen.IsActive(); c105 = (weight.rgb * blend, weight.a).
        out.motionBlur = pp->DoMotionBlur();
        if (out.motionBlur) {
            const Hmx::Color &mw = pp->GetMotionBlurWeight();
            const float blend = pp->GetMotionBlurBlend();
            out.motionBlurParams[0] = mw.red * blend;
            out.motionBlurParams[1] = mw.green * blend;
            out.motionBlurParams[2] = mw.blue * blend;
            out.motionBlurParams[3] = mw.alpha;
        }
        // CheckHueConverge (DoHueConverge() is the merged_Returns1 ICF stub,
        // always true): c222 = (hue_target / 360 + 0.5, hue_focus,
        // blend_amount, brightness_power); bit 62 (unk2a) = blend_amount > 0.
        const float hueBlend = pp->GetHueBlendAmount();
        out.hueConverge = hueBlend > 0.0f;
        if (out.hueConverge) {
            out.hueParams[0] = pp->GetHueTarget() * (1.0f / 360.0f) + 0.5f;
            out.hueParams[1] = pp->GetHueFocus();
            out.hueParams[2] = hueBlend;
            out.hueParams[3] = pp->GetHueBrightnessPower();
        }
        // The noise map, seeds and scales are filled by PostProcPass, which
        // owns the stationary seed (NgPostProc::mRandomSeed).
    }
}
} // namespace

namespace {
// lane A55: RB3's own X360 chain (kRetailPostChain, rb3-xenon), as the
// RB3-Wii shape fills it (rndshape/RB3WiiPostChain.cpp) but with the colour
// modulation the game computed: retail band.exe TU5 NgPostProc::DoPost
// (fn_82B89A08) runs RndPostProc::DoPost (UpdateColorModulation) and then
// ModulateColorXfm, which scales the RndColorXfm 3x3 by mColorModulation
// (+0x12C) before it loads c92..c94. DoBloom (fn_82B884A0): doBloom =
// BloomIntensity() > 0 || bloomColor.alpha > 0, c6 = bloomColor.rgb *
// BloomIntensity(). The texture noise is NOT taken into this chain: RB3's
// composite (gfx/RB3RetailPost) has only the generic composite's procedural
// grain, which lane A49 measured as the wrong noise for rb3-xenon (retail
// CheckNoise, fn_82B89100, samples mNoiseMap), so it stays at 0 here.
template <class P>
void FillRB3(const P *pp, RetailPostParams &out) {
    if constexpr (!PostProcHasColorModulation<P>::value || PostProcHasBloomLocFactor<P>::value) {
        (void)pp;
        (void)out;
    } else {
        const float intensity = pp->BloomIntensity();
        const Hmx::Color &bc = pp->GetBloomColor();
        out.bloom = intensity > 0.0f || bc.alpha > 0.0f;
        out.bloomColor[0] = bc.red * intensity;
        out.bloomColor[1] = bc.green * intensity;
        out.bloomColor[2] = bc.blue * intensity;
        out.bloomLumaScale = BloomLumaScale(pp);
        const float mod = pp->GetColorModulation();
        const Transform &t = pp->GetColorXfm().mColorXfm;
        const Vector3 *rows[3] = {&t.m.x, &t.m.y, &t.m.z};
        for (int i = 0; i < 3; i++) {
            const float vi = i == 0 ? t.v.x : i == 1 ? t.v.y : t.v.z;
            for (int j = 0; j < 3; j++) {
                const Vector3 &r = *rows[j];
                out.xfm[i][j] = (i == 0 ? r.x : i == 1 ? r.y : r.z) * mod;
            }
            out.xfm[i][3] = vi;
        }
        out.vignetteIntensity = pp->GetVignetteIntensity();
        const Hmx::Color &vc = pp->GetVignetteColor();
        out.vignetteColor[0] = vc.red;
        out.vignetteColor[1] = vc.green;
        out.vignetteColor[2] = vc.blue;
        out.vignetteColor[3] = vc.alpha;
        out.posterLevels = pp->GetPosterLevels();
        out.posterMin = pp->GetPosterMin();
        out.chromaticOffset = pp->GetChromaticAberrationOffset();
        out.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
        out.noiseIntensity = 0.0f;
        out.noiseMidtone = 0.0f;
    }
}
} // namespace

void FillRetailPost(const RndPostProc *pp, float flickerMul, RetailPostParams &out) {
    out = RetailPostParams();
    if (!pp) return;
    if constexpr (kRetailPostChain) {
        (void)flickerMul;
        FillRB3(pp, out);
    } else {
        FillDC3(pp, flickerMul, out);
    }
}

} // namespace rndshape
