// LookProfile.cpp — DC3_LOOK, resolved once. See LookProfile.h.
#include "platform/LookProfile.h"
#include "platform/rndshape/RndShape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace milo {

namespace {

// np-look34: the enhanced look's text bias (see README of lane item34b).
constexpr float kEnhancedUiLodBias = -1.0f;

// np-look49: the Apple class's value of each per-adapter rule in the enhanced
// look (LookAdapterClass::Apple), and its two numbers of the texture pass's
// size rule (the cap on the longer side, the base rows). Timed on an M3
// MacBook Air (Metal) by the landing session: with both texture pass steps a
// 2048 target cost +0.48 to +0.50 ms of frame span against a limit of +0.30
// (fails). kAppleTexPassMaxSide 1536: measured +0.27 to +0.29 against +0.30
// on the M3, 2026-11-21 (1080 and 1440 output rows), so anything that adds
// to this pass on an Apple GPU needs a new reading. A 1024 projection is 1024
// (4 samples, never below its own size) up to 720 output rows, 1536 from
// 1080 up. No Apple GPU other than the M3 was timed. One line each to change.
constexpr bool kAppleDofNearField = true;
constexpr bool kAppleDofNearSpread = true;
// np-look51: the depth of field's edge rule on an Apple GPU. Off: timed on
// the M3 on 2026-11-22 it is over its limit at every size (+0.23 to +0.25 ms
// against +0.10 at 1268x713, +0.32 to +0.35 against +0.20 at 1920x1080,
// +0.53 against +0.35 at 2560x1440; dc3 PLAN H-180, the addendum). The
// picture on Metal is correct, so this is a cost decision only.
constexpr bool kAppleDofEdge = false;
constexpr bool kAppleTexPassMsaa = true;
constexpr bool kAppleTexPassScale = true;
constexpr int kAppleTexPassMaxSide = 1536;
constexpr int kAppleTexPassBaseRows = 720;
// np-look56: the Apple class in a browser (ClassifyWebLookAdapter: vendor
// "apple" with texture-compression-bc, not a fallback adapter, i.e. an Apple
// silicon Mac). The class's values above were timed natively on the M3 (Dawn
// on Metal). In headless Chrome on the M3 the class draws correctly, but the
// game page makes about 40 frames a second there in every class (the landing
// session's run, 2026-11-22: dc3 PLAN H-184), and Safari, another WebGPU
// implementation the vendor string cannot tell from Chrome, was not run. Off
// until both are settled: such a Mac is other non-discrete, as before.
constexpr bool kAppleInBrowser = false;

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
    // np-look47 step 1: a render-to-texture target drawn like the frame
    // (LookProfile.h), 4x MSAA resolved into it. On a discrete GPU. Off on
    // anything else until a handheld's timing says it fits: measured on the
    // 3090, it adds ~0.025 ms to the texture pass at 1024x1024; scaled by the
    // 890M's fill and bandwidth (8-15x) that is 0.19-0.36 ms, over the 0.15 ms
    // a 1268x713 Break It Down frame may take (item 47 README). An Apple GPU:
    // kAppleTexPassMsaa (np-look49).
    s.texPassMsaa = enh;
    s.texPassMsaaApple = enh && kAppleTexPassMsaa;
    s.texPassMsaaIntegrated = false;
    // np-look47 step 2: the target's GPU texture sized for the output's rows.
    // On a discrete GPU only, as step 1: on the 3090 steps 1+2 add ~0.05 ms at
    // 1536 (540 rows) and ~0.10 ms at 2048 (720 rows and above); 144 MiB a
    // target at 2048 with 4 samples (36 at 1024). An Apple GPU:
    // kAppleTexPassScale, with its own cap and rows (below).
    s.texPassScale = enh;
    s.texPassScaleApple = enh && kAppleTexPassScale;
    s.texPassScaleIntegrated = false;
    // The size rule's two numbers per class, in every profile (they only act
    // with texPassScale). The Integrated pair is the discrete one: unused
    // while texPassScaleIntegrated is off, and a forced-on handheld gets the
    // discrete class's size, as it did with one pair.
    s.texPassMaxSideApple = kAppleTexPassMaxSide;
    s.texPassBaseRowsApple = kAppleTexPassBaseRows;
    s.texPassMaxSideIntegrated = s.texPassMaxSide;
    s.texPassBaseRowsIntegrated = s.texPassBaseRows;
    // np-look23 (a): the noise map on whole output rows (LookProfile.h).
    // Uniforms only, no cost but a memoised constant per size.
    s.noiseSnapsToOutputRows = enh;
    // np-look23 (c): the depth of field gathers by depth (LookProfile.h).
    s.postDofDepthWeight = enh;
    // np-look36: the depth weight's near field (LookProfile.h).
    // The near field and its spread by adapter class: on a discrete GPU; off
    // on anything else until a handheld's timing says each fits there. An
    // Apple GPU: kAppleDofNearField / kAppleDofNearSpread (np-look49).
    s.postDofNearField = enh;
    s.postDofNearFieldApple = enh && kAppleDofNearField;
    s.postDofNearFieldIntegrated = false;
    s.postDofNearSpread = enh;
    s.postDofNearSpreadApple = enh && kAppleDofNearSpread;
    s.postDofNearSpreadIntegrated = false;
    // np-look51: the edge rule (LookProfile.h). On a discrete GPU; Apple's is
    // kAppleDofEdge (off, over the M3's limit); anything else off, the 890M
    // not being timed.
    s.postDofEdge = enh;
    s.postDofEdgeApple = enh && kAppleDofEdge;
    s.postDofEdgeIntegrated = false;
    // np-present60: the clamped cubic upscale in the present (LookProfile.h).
    // On a discrete GPU and on anything else (the 890M timed it under its
    // limit, np-present60b); Apple's off, the M3 not being timed.
    s.presentUpscale = enh;
    s.presentUpscaleApple = false;
    s.presentUpscaleIntegrated = enh;
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

bool SetLookDofNearForInspection(int mode, bool spread) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    // Each has a value per adapter class. On and off force all three, so the
    // token means the same on any GPU; the rule returns each to the profile's.
    if (!spread) {
        s.postDofNearField = mode < 0 ? r.postDofNearField : mode != 0;
        s.postDofNearFieldIntegrated = mode < 0 ? r.postDofNearFieldIntegrated : mode != 0;
        s.postDofNearFieldApple = mode < 0 ? r.postDofNearFieldApple : mode != 0;
        fprintf(stderr,
                "[look] depth of field near field %d, %d on a non-discrete GPU, %d on an Apple GPU%s "
                "(inspection, profile %s)\n",
                (int)s.postDofNearField, (int)s.postDofNearFieldIntegrated, (int)s.postDofNearFieldApple,
                mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
        return true;
    }
    s.postDofNearSpread = mode < 0 ? r.postDofNearSpread : mode != 0;
    s.postDofNearSpreadIntegrated = mode < 0 ? r.postDofNearSpreadIntegrated : mode != 0;
    s.postDofNearSpreadApple = mode < 0 ? r.postDofNearSpreadApple : mode != 0;
    fprintf(stderr,
            "[look] depth of field near spread %d, %d on a non-discrete GPU, %d on an Apple GPU%s "
            "(inspection, profile %s)\n",
            (int)s.postDofNearSpread, (int)s.postDofNearSpreadIntegrated, (int)s.postDofNearSpreadApple,
            mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookPresentUpscaleForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    s.presentUpscale = mode < 0 ? r.presentUpscale : mode != 0;
    s.presentUpscaleIntegrated = mode < 0 ? r.presentUpscaleIntegrated : mode != 0;
    s.presentUpscaleApple = mode < 0 ? r.presentUpscaleApple : mode != 0;
    fprintf(stderr,
            "[look] present upscale (clamped Catmull-Rom, 5 reads + 3 gathers, for the one tap) %d, %d on a "
            "non-discrete GPU, %d on an Apple GPU%s (inspection, profile %s)\n",
            (int)s.presentUpscale, (int)s.presentUpscaleIntegrated, (int)s.presentUpscaleApple,
            mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookDofEdgeForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    s.postDofEdge = mode < 0 ? r.postDofEdge : mode != 0;
    s.postDofEdgeIntegrated = mode < 0 ? r.postDofEdgeIntegrated : mode != 0;
    s.postDofEdgeApple = mode < 0 ? r.postDofEdgeApple : mode != 0;
    fprintf(stderr,
            "[look] depth of field edge rule %d, %d on a non-discrete GPU, %d on an Apple GPU%s "
            "(inspection, profile %s)\n",
            (int)s.postDofEdge, (int)s.postDofEdgeIntegrated, (int)s.postDofEdgeApple,
            mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

LookAdapterClass ClassifyLookAdapter(LookAdapterType type, uint32_t vendorId, bool metalBackend) {
    if (type == LookAdapterType::Discrete) return LookAdapterClass::Discrete;
    // A CPU or unknown adapter is never Apple, whoever reports it.
    if (type == LookAdapterType::Integrated && vendorId == kAppleGpuVendorId && metalBackend)
        return LookAdapterClass::Apple;
    return LookAdapterClass::OtherNonDiscrete;
}

LookAdapterClass ClassifyWebLookAdapter(const char *vendor, bool hasBC, bool fallback, bool appleInBrowser) {
    // A fallback adapter is a software rasteriser, whoever's name it carries.
    if (fallback || !vendor) return LookAdapterClass::OtherNonDiscrete;
    // Exact, case-sensitive: the specification's normalised identifiers are
    // lower case (LookProfile.h). NVIDIA's browser-capable GPUs are discrete.
    if (strcmp(vendor, "nvidia") == 0) return LookAdapterClass::Discrete;
    // Without BC it is an iPhone or iPad, not a Mac.
    if (strcmp(vendor, "apple") == 0 && hasBC && appleInBrowser) return LookAdapterClass::Apple;
    return LookAdapterClass::OtherNonDiscrete;
}

bool LookAppleInBrowser() { return kAppleInBrowser; }

LookAdapterClass ClassifyLookAdapter(const LookAdapterFacts &f) {
    if (f.web) return ClassifyWebLookAdapter(f.webVendor, f.webHasBC, f.webFallback, kAppleInBrowser);
    return ClassifyLookAdapter(f.type, f.vendorId, f.metalBackend);
}

void SetLookAdapterWebStrings(LookAdapterFacts &f, const char *vendor, size_t vendorLen, const char *architecture,
                              size_t architectureLen) {
    const auto copy = [](char *dst, const char *src, size_t len) {
        // SIZE_MAX (WGPU_STRLEN): NUL-terminated.
        if (src && len == (size_t)-1) len = strnlen(src, kLookAdapterWebStringSize);
        const size_t n = src ? std::min(len, (size_t)kLookAdapterWebStringSize - 1) : 0;
        if (n) memcpy(dst, src, n);
        dst[n] = '\0';
    };
    copy(f.webVendor, vendor, vendorLen);
    copy(f.webArchitecture, architecture, architectureLen);
}

namespace {
// What the class was decided from, for the two class lines: the native
// facts, or the browser's.
void DescribeLookAdapter(const LookAdapterFacts &a, char *buf, size_t size) {
    const char *backend = a.backendName ? a.backendName : "unknown";
    if (a.web)
        snprintf(buf, size, "browser vendor \"%s\", architecture \"%s\", texture-compression-bc %d, fallback %d, "
                 "backend %s", a.webVendor, a.webArchitecture, (int)a.webHasBC, (int)a.webFallback, backend);
    else
        snprintf(buf, size, "type %s, vendor 0x%04x, backend %s", LookAdapterTypeName(a.type), (unsigned)a.vendorId,
                 backend);
}
}  // namespace

const char *LookAdapterClassName(LookAdapterClass c) {
    switch (c) {
    case LookAdapterClass::Discrete: return "discrete GPU";
    case LookAdapterClass::Apple: return "Apple GPU (not a discrete GPU)";
    case LookAdapterClass::OtherNonDiscrete: break;
    }
    return "not a discrete GPU";
}

const char *LookAdapterTypeName(LookAdapterType t) {
    switch (t) {
    case LookAdapterType::Discrete: return "discrete";
    case LookAdapterType::Integrated: return "integrated";
    case LookAdapterType::Cpu: return "cpu";
    case LookAdapterType::Unknown: break;
    }
    return "unknown";
}

namespace {
// The one place a per-adapter rule's three values are chosen between.
bool PickByClass(LookAdapterClass c, bool discrete, bool apple, bool other) {
    switch (c) {
    case LookAdapterClass::Discrete: return discrete;
    case LookAdapterClass::Apple: return apple;
    case LookAdapterClass::OtherNonDiscrete: break;
    }
    return other;
}
// The same for a per-class number.
int PickByClass(LookAdapterClass c, int discrete, int apple, int other) {
    switch (c) {
    case LookAdapterClass::Discrete: return discrete;
    case LookAdapterClass::Apple: return apple;
    case LookAdapterClass::OtherNonDiscrete: break;
    }
    return other;
}
}  // namespace

bool LookDofNearField(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.postDofNearField, look.postDofNearFieldApple, look.postDofNearFieldIntegrated);
}

bool LookDofNearSpread(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.postDofNearSpread, look.postDofNearSpreadApple, look.postDofNearSpreadIntegrated);
}

bool LookDofEdge(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.postDofEdge, look.postDofEdgeApple, look.postDofEdgeIntegrated);
}

bool LookPresentUpscale(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.presentUpscale, look.presentUpscaleApple, look.presentUpscaleIntegrated);
}

void PrintLookAdapterClass(const LookAdapterFacts &adapter) {
    const LookSettings &s = Look();
    if (s.profile == LookProfile::Native) return;
    const LookAdapterClass c = ClassifyLookAdapter(adapter);
    char from[192];
    DescribeLookAdapter(adapter, from, sizeof(from));
    fprintf(stderr,
            "[look] adapter class: %s; depth of field near field %d, near spread %d, edge %d, texture pass "
            "msaa %d, scale %d (cap %d, base rows %d) [%s] (profile %s)\n",
            LookAdapterClassName(c), (int)LookDofNearField(s, c), (int)LookDofNearSpread(s, c),
            (int)LookDofEdge(s, c), (int)LookTexPassMsaa(s, c), (int)LookTexPassScale(s, c), LookTexPassMaxSide(s, c),
            LookTexPassBaseRows(s, c), from, LookProfileName(s.profile));
}

// The adapter LogLookAdapterClass was given, and the class token's choice
// (-1 = the adapter's own, else a LookAdapterClass).
static LookAdapterFacts sLoggedAdapter;
static int sAdapterClassInspection = -1;

void LogLookAdapterClass(const LookAdapterFacts &adapter) {
    static bool sLogged = false;
    sLoggedAdapter = adapter;
    if (sLogged || Look().profile == LookProfile::Native) return;
    sLogged = true;
    PrintLookAdapterClass(adapter);
}

LookAdapterFacts LoggedLookAdapter() { return sLoggedAdapter; }

LookAdapterClass LookAdapterClassInEffect(const LookAdapterFacts &adapter) {
    if (sAdapterClassInspection >= 0) return (LookAdapterClass)sAdapterClassInspection;
    return ClassifyLookAdapter(adapter);
}

bool SetLookAdapterClassForInspection(bool useAdapter, LookAdapterClass forced) {
    const LookSettings &s = Look();
    if (s.profile == LookProfile::Native) return false;
    if (useAdapter) {
        sAdapterClassInspection = -1;
        PrintLookAdapterClass(sLoggedAdapter);
        return true;
    }
    sAdapterClassInspection = (int)forced;
    const LookAdapterFacts &a = sLoggedAdapter;
    const LookAdapterClass own = ClassifyLookAdapter(a);
    const char *is = own == LookAdapterClass::Discrete ? "a discrete GPU"
                     : own == LookAdapterClass::Apple  ? "an Apple GPU (not a discrete GPU)"
                                                       : "not a discrete GPU";
    char from[192];
    DescribeLookAdapter(a, from, sizeof(from));
    fprintf(stderr,
            "[look] adapter class forced for inspection: %s; the adapter is %s [%s]; depth of field near "
            "field %d, near spread %d, edge %d, texture pass msaa %d, "
            "scale %d (cap %d, base rows %d) (profile %s)\n",
            LookAdapterClassName(forced), is, from, (int)LookDofNearField(s, forced),
            (int)LookDofNearSpread(s, forced), (int)LookDofEdge(s, forced), (int)LookTexPassMsaa(s, forced), (int)LookTexPassScale(s, forced),
            LookTexPassMaxSide(s, forced), LookTexPassBaseRows(s, forced), LookProfileName(s.profile));
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

bool SetLookTexPassMsaaForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    s.texPassMsaa = mode < 0 ? r.texPassMsaa : mode != 0;
    s.texPassMsaaIntegrated = mode < 0 ? r.texPassMsaaIntegrated : mode != 0;
    s.texPassMsaaApple = mode < 0 ? r.texPassMsaaApple : mode != 0;
    fprintf(stderr,
            "[look] texture pass msaa %d, %d on a non-discrete GPU, %d on an Apple GPU%s (inspection, profile %s)\n",
            (int)s.texPassMsaa, (int)s.texPassMsaaIntegrated, (int)s.texPassMsaaApple,
            mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookTexPassScaleForInspection(int mode) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    s.texPassScale = mode < 0 ? r.texPassScale : mode != 0;
    s.texPassScaleIntegrated = mode < 0 ? r.texPassScaleIntegrated : mode != 0;
    s.texPassScaleApple = mode < 0 ? r.texPassScaleApple : mode != 0;
    fprintf(stderr,
            "[look] texture pass scale %d, %d on a non-discrete GPU, %d on an Apple GPU%s (inspection, profile %s)\n",
            (int)s.texPassScale, (int)s.texPassScaleIntegrated, (int)s.texPassScaleApple,
            mode < 0 ? " (the rule)" : "", LookProfileName(s.profile));
    return true;
}

bool SetLookTexPassSizeForInspection(int maxSide, int baseRows) {
    LookSettings &s = Mutable();
    if (s.profile == LookProfile::Native) return false;
    const LookSettings &r = SettingsFor(s.profile);
    // A number > 0 is set for all three classes, < 0 returns each to its own.
    if (maxSide != 0) {
        s.texPassMaxSide = maxSide < 0 ? r.texPassMaxSide : maxSide;
        s.texPassMaxSideIntegrated = maxSide < 0 ? r.texPassMaxSideIntegrated : maxSide;
        s.texPassMaxSideApple = maxSide < 0 ? r.texPassMaxSideApple : maxSide;
    }
    if (baseRows != 0) {
        s.texPassBaseRows = baseRows < 0 ? r.texPassBaseRows : baseRows;
        s.texPassBaseRowsIntegrated = baseRows < 0 ? r.texPassBaseRowsIntegrated : baseRows;
        s.texPassBaseRowsApple = baseRows < 0 ? r.texPassBaseRowsApple : baseRows;
    }
    const bool capRule = s.texPassMaxSide == r.texPassMaxSide && s.texPassMaxSideIntegrated == r.texPassMaxSideIntegrated
                         && s.texPassMaxSideApple == r.texPassMaxSideApple;
    const bool rowsRule = s.texPassBaseRows == r.texPassBaseRows
                          && s.texPassBaseRowsIntegrated == r.texPassBaseRowsIntegrated
                          && s.texPassBaseRowsApple == r.texPassBaseRowsApple;
    fprintf(stderr,
            "[look] texture pass size cap %d, %d on a non-discrete GPU, %d on an Apple GPU%s, base rows %d, %d on a "
            "non-discrete GPU, %d on an Apple GPU%s (inspection, profile %s)\n",
            s.texPassMaxSide, s.texPassMaxSideIntegrated, s.texPassMaxSideApple, capRule ? " (the rule)" : "",
            s.texPassBaseRows, s.texPassBaseRowsIntegrated, s.texPassBaseRowsApple, rowsRule ? " (the rule)" : "",
            LookProfileName(s.profile));
    return true;
}

bool LookTexPassScale(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.texPassScale, look.texPassScaleApple, look.texPassScaleIntegrated);
}

int LookTexPassMaxSide(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.texPassMaxSide, look.texPassMaxSideApple, look.texPassMaxSideIntegrated);
}

int LookTexPassBaseRows(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.texPassBaseRows, look.texPassBaseRowsApple, look.texPassBaseRowsIntegrated);
}

void TexPassGpuSize(const LookSettings &look, LookAdapterClass c, int texW, int texH, int outputRows, int &gpuW,
                    int &gpuH) {
    gpuW = texW;
    gpuH = texH;
    const int baseRows = LookTexPassBaseRows(look, c), maxSide = LookTexPassMaxSide(look, c);
    if (texW <= 0 || texH <= 0 || outputRows <= 0 || baseRows <= 0) return;
    float k = (float)outputRows / (float)baseRows;
    const int side = std::max(texW, texH);
    if (maxSide > 0 && side * k > (float)maxSide)
        k = (float)maxSide / (float)side;
    if (!(k > 1.0f)) return;
    gpuW = std::max(texW, (int)(texW * k + 0.5f));
    gpuH = std::max(texH, (int)(texH * k + 0.5f));
}

bool LookTexPassMsaa(const LookSettings &look, LookAdapterClass c) {
    return PickByClass(c, look.texPassMsaa, look.texPassMsaaApple, look.texPassMsaaIntegrated);
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
    : mPrev(Mutable()), mPrevStartupBitmapMipLevels(sStartupBitmapMipLevels),
      mPrevAdapterClassInspection(sAdapterClassInspection) {
    Mutable() = SettingsFor(p);
    sStartupBitmapMipLevels = Mutable().bitmapMipLevels;  // a scope stands for a process started in p
    sCutoutModeInspection = 0;
    sAdapterClassInspection = -1;  // the adapter's own class
}

ScopedLookOverride::~ScopedLookOverride() {
    Mutable() = mPrev;
    sStartupBitmapMipLevels = mPrevStartupBitmapMipLevels;
    sCutoutModeInspection = 0;
    sAdapterClassInspection = mPrevAdapterClassInspection;
}

}  // namespace milo
