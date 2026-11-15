// LookProfile.cpp — DC3_LOOK, resolved once. See LookProfile.h.
#include "platform/LookProfile.h"
#include "platform/rndshape/RndShape.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace milo {

namespace {

LookSettings SettingsFor(LookProfile p) {
    LookSettings s;
    s.profile = p;
    s.bitmapMipLevels = rndshape::kBitmapMaxMipLevels;
    if (p == LookProfile::Retail || p == LookProfile::Enhanced) s.cubeMipChain = true;
    if (p == LookProfile::Enhanced) {
        s.bitmapMipLevels = 0;  // whole authored chain ...
        s.maxAnisotropy = 16;   // ... filtered anisotropically
        s.uiMipLevels = 2;      // text/HUD keep the original's two levels
        // World textures one level sharper: with the whole chain under 16x
        // aniso this is as sharp as the original's two-level texture (which
        // stays at its base level, aliasing) and steadier frame to frame.
        // Text and HUD stay unbiased (PipelineKey::lodBias).
        s.lodBias = -1.0f;
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

LookSettings &Mutable() {
    static LookSettings s = Resolve();
    return s;
}

}  // namespace

const LookSettings &Look() { return Mutable(); }

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

bool SetLookProfileForInspection(LookProfile p) {
    if (Mutable().profile == LookProfile::Native || p == LookProfile::Native) return false;
    Mutable() = SettingsFor(p);
    fprintf(stderr, "[look] profile %s (inspection)\n", LookProfileName(p));
    return true;
}

ScopedLookOverride::ScopedLookOverride(LookProfile p) : mPrev(Mutable()) {
    Mutable() = SettingsFor(p);
}

ScopedLookOverride::~ScopedLookOverride() { Mutable() = mPrev; }

}  // namespace milo
