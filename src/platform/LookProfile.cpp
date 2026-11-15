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
        fprintf(stderr, "[look] profile %s\n", LookProfileName(p));
        return SettingsFor(p);
    }
}

LookSettings &Mutable() {
    static LookSettings s = Resolve();
    return s;
}

}  // namespace

const LookSettings &Look() { return Mutable(); }

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
