// LookProfile.cpp — DC3_LOOK, resolved once. See LookProfile.h.
#include "platform/LookProfile.h"
#include "platform/rndshape/RndShape.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace milo {

namespace {

// np-look34: the enhanced look's text bias (see README of lane item34b).
constexpr float kEnhancedUiLodBias = -1.0f;

LookSettings SettingsFor(LookProfile p) {
    LookSettings s;
    s.profile = p;
    s.bitmapMipLevels = rndshape::kBitmapMaxMipLevels;
    if (p == LookProfile::Retail || p == LookProfile::Enhanced) s.cubeMipChain = true;
    if (p == LookProfile::Enhanced) {
        s.bitmapMipLevels = 0;  // whole authored chain ...
        s.maxAnisotropy = 16;   // ... filtered anisotropically
        s.uiMipLevels = 2;      // text/HUD keep the original's two levels
        // np-look34: the text and HUD bias (LookSettings::uiLodBias). Pipelines
        // read it so the tokens can move it; the value is set below.
        s.uiTakesLodBias = true;
        s.uiLodBias = kEnhancedUiLodBias;
        s.uiLodBiasGlyphsOnly = true;
        // World textures: the LOD bias by render scale (MaterialLodBiasAt):
        // 0 at both ends (item 19 a, measured on the 3090 with the drift
        // protocol, three venues). At scale 1, -1 raised frame shimmer
        // x1.145 / x1.079 / x1.036 / x1.010 at 720 / 1080 / 1440 / 2160
        // rows for x1.10 / x1.06 / x1.03 / x1.01 still sharpness, and at 0
        // the look is already as sharp as the retail cap (boombox 1.025-
        // 1.048x, frame 1.12-1.24x). At scale 2 the 2x2 box averages one
        // level, and -1 bought +0.2-0.8% sharpness for +0.6-1.8% shimmer
        // (boombox +7-11%). Round 2's -1 was picked against the aliased cap
        // with a near-still camera. The rule stays per scale and the
        // pipelines stay biasable (materialLodBias) so it can be moved, and
        // inspected live, without a compile. Text and HUD stay unbiased
        // (PipelineKey::lodBias).
        s.lodBiasScale1 = 0.0f;
        s.lodBiasScale2 = 0.0f;
        s.materialLodBias = true;
        // 2x2 supersampling on a discrete GPU (~4x the main pass's fill);
        // none on an integrated one, where the main pass alone is ~2 ms.
        s.renderScale = 2.0f;
        s.renderScaleIntegrated = 1.0f;
        // The frame at the window's resolution, at most 1080 rows on an
        // integrated GPU (a handheld's 2560x1600 panel is 4x a 720p frame).
        s.outputFollowsTarget = true;
        s.outputMaxRowsIntegrated = 1080;
        // Web, on a phone-class adapter (no texture-compression-bc): 720.
        s.outputMaxRowsNoBc = 720;
    }
    // np-gfx-post round 10 (LookProfile.h): every platform, web and
    // integrated GPUs included. Measured on a 3090 at 1x, the three together add
    // 0.017-0.027 ms to the post chain, +0.006 ms on DOF frames; the 890M estimate
    // is 0.15-0.25 ms of a 0.36 ms budget (doc §37.11).
    const bool enh = p == LookProfile::Enhanced;
    s.postPrecision = enh;
    s.postBloomFilter = enh;
    s.postDofFilter = enh;
    s.specularAA = enh;
    // np-look32: soft contact shadows under the crowd (LookSettings::crowdContactShadow).
    s.crowdContactShadow = enh;
    // np-look23 (a): the noise map on whole output rows (LookProfile.h).
    // Uniforms only, no cost but a memoised constant per size.
    s.noiseSnapsToOutputRows = enh;
    // np-look23 (c): the depth of field gathers by depth (LookProfile.h).
    s.postDofDepthWeight = enh;
    // np-look26 g: cutoutSampleTest stays off in every look. Measured against
    // the landed sharpened coverage it is a trade, not a gain (LookProfile.h);
    // the hair_edge inspection tokens turn it on.
    return s;
}

LookSettings Resolve() {
    if constexpr (!rndshape::kDC3RetailModel) {
        return SettingsFor(LookProfile::Native);
    } else {
        const char *e = getenv("DC3_LOOK");
        LookProfile p = LookProfile::Enhanced;
        if (e && *e) {
            if (strcmp(e, "retail") == 0) p = LookProfile::Retail;
            else if (strcmp(e, "enhanced") != 0)
                fprintf(stderr, "[look] DC3_LOOK=%s is not retail|enhanced; using enhanced\n", e);
        }
        LookSettings s = SettingsFor(p);
        const char *rs = getenv("MILO_RENDER_SCALE");
        bool bad = false;
        s.renderScaleExplicit = ParseRenderScaleKnob(rs, &bad);
        if (bad)
            fprintf(stderr, "[look] MILO_RENDER_SCALE=%s is not a number > 0; using the "
                    "profile default\n", rs);
        const char *rows = getenv("MILO_OUTPUT_MAX_ROWS");
        s.outputMaxRowsExplicit = ParseOutputRowsKnob(rows, &bad);
        if (bad)
            fprintf(stderr, "[look] MILO_OUTPUT_MAX_ROWS=%s is not a whole number >= 0; using "
                    "the profile default\n", rows);
        char knobs[96] = "";
        int n = 0;
        if (s.renderScaleExplicit > 0.0f)
            n += snprintf(knobs + n, sizeof(knobs) - n, ", render scale %.2f (MILO_RENDER_SCALE)",
                          s.renderScaleExplicit);
        if (s.outputMaxRowsExplicit >= 0)
            snprintf(knobs + n, sizeof(knobs) - n, ", output rows %s (MILO_OUTPUT_MAX_ROWS)",
                     s.outputMaxRowsExplicit ? rows : "not capped");
        fprintf(stderr, "[look] profile %s%s\n", LookProfileName(p), knobs);
        return s;
    }
}

// The bitmapMipLevels the process's regular textures were uploaded with: the
// look it started in (StartupBitmapMipLevels). Set once, by the first Look().
int sStartupBitmapMipLevels = 0;

LookSettings &Mutable() {
    static LookSettings s = [] {
        LookSettings r = Resolve();
        sStartupBitmapMipLevels = r.bitmapMipLevels;
        return r;
    }();
    return s;
}

}  // namespace

const LookSettings &Look() { return Mutable(); }

int StartupBitmapMipLevels() {
    Mutable();
    return sStartupBitmapMipLevels;
}

int LookSwitchTexLevels(const LookSettings &look, int startupBitmapMipLevels) {
    if (look.bitmapMipLevels <= 0 || look.bitmapMipLevels == startupBitmapMipLevels) return 0;
    return look.bitmapMipLevels;
}

float ParseRenderScaleKnob(const char *value, bool *bad) {
    if (bad) *bad = false;
    if (!value || !*value) return 0.0f;
    char *end = nullptr;
    const float v = strtof(value, &end);
    if (end == value || *end != '\0' || !(v > 0.0f)) {  // also rejects NaN
        if (bad) *bad = true;
        return 0.0f;
    }
    return v < 1.0f ? 1.0f : v > 2.0f ? 2.0f : v;
}

int ParseOutputRowsKnob(const char *value, bool *bad) {
    if (bad) *bad = false;
    if (!value || !*value) return -1;
    char *end = nullptr;
    const long v = strtol(value, &end, 10);
    if (end == value || *end != '\0' || v < 0 || v > 16384) {
        if (bad) *bad = true;
        return -1;
    }
    return (int)v;
}

const char *LookProfileName(LookProfile p) {
    switch (p) {
    case LookProfile::Retail: return "retail";
    case LookProfile::Enhanced: return "enhanced";
    case LookProfile::Native: break;
    }
    return "native";
}

bool SetLookPostFieldsForInspection(bool precision, bool bloomFilter, bool dofFilter) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.postPrecision = precision;
    s.postBloomFilter = bloomFilter;
    s.postDofFilter = dofFilter;
    fprintf(stderr, "[look] post fields precision %d bloom filter %d dof filter %d (inspection, profile %s)\n",
            (int)precision, (int)bloomFilter, (int)dofFilter, LookProfileName(s.profile));
    return true;
}

bool SetLookPostForInspection(bool enhanced) {
    return SetLookPostFieldsForInspection(enhanced, enhanced, enhanced);
}

bool SetLookSpecularAAForInspection(bool on) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.specularAA = on;
    fprintf(stderr, "[look] specular AA %d (inspection, profile %s)\n", (int)on, LookProfileName(s.profile));
    return true;
}

bool LookTakesLodBias(const LookSettings &look) {
    return look.materialLodBias || look.lodBiasScale1 != 0.0f || look.lodBiasScale2 != 0.0f;
}

float MaterialLodBiasAt(const LookSettings &look, float scale) {
    if (look.lodBiasInspected) return look.lodBiasInspect;
    // !(scale > 1) also catches NaN: an unknown scale is the unscaled frame.
    const float t = !(scale > 1.0f) ? 0.0f : scale >= 2.0f ? 1.0f : log2f(scale);
    return look.lodBiasScale1 + (look.lodBiasScale2 - look.lodBiasScale1) * t;
}

float PassMaterialLodBias(const LookSettings &look, bool toTextureTarget, float frameRenderScale) {
    return MaterialLodBiasAt(look, toTextureTarget ? 1.0f : frameRenderScale);
}

bool SetLookLodBiasForInspection(bool useRule, float bias) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.lodBiasInspected = !useRule;
    s.lodBiasInspect = useRule ? 0.0f : bias;
    if (useRule)
        fprintf(stderr, "[look] lod bias: the rule, %.2f at 1x / %.2f at 2x (inspection, profile %s)\n",
                s.lodBiasScale1, s.lodBiasScale2, LookProfileName(s.profile));
    else
        fprintf(stderr, "[look] lod bias %.2f at every scale (inspection, profile %s)\n", bias,
                LookProfileName(s.profile));
    return true;
}

bool LookTakesUiLodBias(const LookSettings &look) { return look.uiTakesLodBias; }

float UiGlyphLodBias(const LookSettings &look) { return look.uiTakesLodBias ? look.uiLodBias : 0.0f; }

float UiArtLodBias(const LookSettings &look) {
    return look.uiTakesLodBias && !look.uiLodBiasGlyphsOnly ? look.uiLodBias : 0.0f;
}

bool SetLookUiLodBiasForInspection(bool useRule, float bias) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.uiLodBias = useRule ? SettingsFor(s.profile).uiLodBias : bias;
    fprintf(stderr, "[look] text lod bias %.2f%s (inspection, profile %s)\n", s.uiLodBias,
            useRule ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookUiMipLevelsForInspection(int levels) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.uiMipLevels = levels < 0 ? SettingsFor(s.profile).uiMipLevels : levels;
    fprintf(stderr, "[look] text mip levels %d%s (inspection, profile %s)\n", s.uiMipLevels,
            levels < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookUiLodBiasScopeForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.uiLodBiasGlyphsOnly = mode < 0 ? SettingsFor(s.profile).uiLodBiasGlyphsOnly : mode != 0;
    fprintf(stderr, "[look] text lod bias on glyphs only %d%s (inspection, profile %s)\n",
            (int)s.uiLodBiasGlyphsOnly, mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookAnisotropyForInspection(unsigned short maxAnisotropy) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    // 0: the profile's own value. Inspection only: the world samplers are
    // looked up per material setup (ApplyLookSampling), so the next frame's
    // draws find a sampler, and a bind group, for the new value -- one cache
    // miss per material on the switch, which a shipped path never takes.
    s.maxAnisotropy = maxAnisotropy ? maxAnisotropy : SettingsFor(s.profile).maxAnisotropy;
    fprintf(stderr, "[look] world max anisotropy %u%s (inspection, profile %s)\n", (unsigned)s.maxAnisotropy,
            maxAnisotropy ? "" : " (the rule)", LookProfileName(s.profile));
    return true;
}

bool SetLookNoiseRowsForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.noiseSnapsToOutputRows = mode < 0 ? SettingsFor(s.profile).noiseSnapsToOutputRows : mode != 0;
    fprintf(stderr, "[look] noise map on output rows %d%s (inspection, profile %s)\n",
            (int)s.noiseSnapsToOutputRows, mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookDofDepthWeightForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.postDofDepthWeight = mode < 0 ? SettingsFor(s.profile).postDofDepthWeight : mode != 0;
    fprintf(stderr, "[look] depth of field depth-weighted %d%s (inspection, profile %s)\n",
            (int)s.postDofDepthWeight, mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

// The mode a hair_edge_b|c|e|centre token chose, 0 = the default mode.
static int sCutoutModeInspection = 0;

float CutoutSampleTestFor(const LookSettings &look) {
    if (!look.cutoutSampleTest) return 0.0f;
    return (float)(sCutoutModeInspection > 0 ? sCutoutModeInspection : kCutoutModeDefault);
}

bool SetLookCutoutModeForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native || mode < kCutoutModeB || mode > kCutoutModeCentre) return false;
    s.cutoutSampleTest = true;
    sCutoutModeInspection = mode;
    fprintf(stderr, "[look] cutout alpha test per sample, mode %d (inspection, profile %s)\n", mode,
            LookProfileName(s.profile));
    return true;
}

bool SetLookCutoutSampleTestForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    sCutoutModeInspection = 0;
    s.cutoutSampleTest = mode < 0 ? SettingsFor(s.profile).cutoutSampleTest : mode != 0;
    fprintf(stderr, "[look] cutout alpha test per sample %d%s (inspection, profile %s)\n",
            (int)s.cutoutSampleTest, mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookCrowdShadowForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    s.crowdContactShadow = mode < 0 ? SettingsFor(s.profile).crowdContactShadow : mode != 0;
    fprintf(stderr, "[look] crowd contact shadow %d%s (inspection, profile %s)\n",
            (int)s.crowdContactShadow, mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

static int sReflectionCullInspection = -1;
void SetReflectionCullForInspection(int mode) {
    sReflectionCullInspection = mode < 0 ? -1 : (mode ? 1 : 0);
    fprintf(stderr, "[look] reflection cull %s (inspection)\n",
            sReflectionCullInspection < 0 ? "rule" : sReflectionCullInspection ? "force" : "flip");
}
int ReflectionCullInspection() { return sReflectionCullInspection; }

bool SetLookProfileForInspection(LookProfile p) {
    if (Mutable().profile == LookProfile::Native || p == LookProfile::Native) return false;
    Mutable() = SettingsFor(p);
    sCutoutModeInspection = 0;  // the profile's own cut-out mode
    fprintf(stderr, "[look] profile %s (inspection)\n", LookProfileName(p));
    return true;
}

ScopedLookOverride::ScopedLookOverride(LookProfile p)
    : mPrev(Mutable()), mPrevStartupBitmapMipLevels(sStartupBitmapMipLevels) {
    Mutable() = SettingsFor(p);
    sStartupBitmapMipLevels = Mutable().bitmapMipLevels;  // a scope stands for a process started in p
    sCutoutModeInspection = 0;
}

ScopedLookOverride::~ScopedLookOverride() {
    Mutable() = mPrev;
    sStartupBitmapMipLevels = mPrevStartupBitmapMipLevels;
    sCutoutModeInspection = 0;
}

}  // namespace milo
