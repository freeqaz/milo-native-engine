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
    // np-gfx-post round 10 (LookProfile.h): every platform, web and
    // integrated GPUs included; the three cost 0.0x ms together (doc §37.11).
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
