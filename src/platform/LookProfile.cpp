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
    }
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
        if (s.renderScaleExplicit > 0.0f)
            fprintf(stderr, "[look] profile %s, render scale %.2f (MILO_RENDER_SCALE)\n",
                    LookProfileName(p), s.renderScaleExplicit);
        else
            fprintf(stderr, "[look] profile %s\n", LookProfileName(p));
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

const char *LookProfileName(LookProfile p) {
    switch (p) {
    case LookProfile::Retail: return "retail";
    case LookProfile::Enhanced: return "enhanced";
    case LookProfile::Native: break;
    }
    return "native";
}

ScopedLookOverride::ScopedLookOverride(LookProfile p) : mPrev(Mutable()) {
    Mutable() = SettingsFor(p);
}

ScopedLookOverride::~ScopedLookOverride() { Mutable() = mPrev; }

}  // namespace milo
