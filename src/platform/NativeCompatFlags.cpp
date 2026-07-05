// NativeCompatFlags.cpp — runtime for the generated NativeCompat flag registry.
// See NativeCompatFlags.h for the design + the presence/truthy correctness hazard,
// and execution/W0.6/PLAN.md. The table is GENERATED: regenerate the .gen.inc via
//   python3 scripts/analysis/native_compat_census.py gen
// Do not hand-edit the .gen.inc.

#include "platform/NativeCompatFlags.h"

#include <cstdlib>  // getenv
#include <cstdio>   // fprintf
#include <cstring>  // strcmp
#include <new>      // std::nothrow

namespace {

// The generated, committed table. Each row is
//   { name, default, FlagClass::…, FlagRead::…, owner, faithfulStatus, docAnchor }
const NativeCompatFlag kFlags[] = {
#include "NativeCompatFlags.gen.inc"
};
const std::size_t kFlagCount = sizeof(kFlags) / sizeof(kFlags[0]);

// Resolve a flag's raw env ONCE, by its declared read mode. This is the single
// place the two legacy idioms live; every call site defers to it.
bool ResolveTriggered(const NativeCompatFlag& f) {
    const char* e = ::getenv(f.name);
    switch (f.read) {
        case FlagRead::Presence:
            return e != nullptr;                       // any value incl. ""/"0"
        case FlagRead::Truthy:
            return e && e[0] && e[0] != '0';           // non-empty, non-"0"
        case FlagRead::Value:
            return e != nullptr;                       // "set" == present
    }
    return false;
}

}  // namespace

NativeCompat::NativeCompat() : mTriggered(nullptr), mCount(kFlagCount) {
    unsigned char* cache =
        kFlagCount ? new (std::nothrow) unsigned char[kFlagCount] : nullptr;
    for (std::size_t i = 0; i < kFlagCount; ++i) {
        const bool trig = ResolveTriggered(kFlags[i]);
        if (cache) cache[i] = trig ? 1u : 0u;
        // Log workarounds whose env is set — i.e. a default-ON workaround being
        // opted OUT, or an opt-in workaround (RB3_NO_SFX) being turned ON. This is
        // the non-default state worth surfacing, mirroring NativeSettings' stderr
        // "[NativeSettings] …" active-override log.
        if (trig && kFlags[i].cls == FlagClass::Workaround) {
            const char* raw = ::getenv(kFlags[i].name);
            fprintf(stderr,
                    "[NativeCompat] override active: %s=%s (default %s, %s)\n",
                    kFlags[i].name, raw ? raw : "", kFlags[i].def, kFlags[i].owner);
        }
    }
    mTriggered = cache;
    if (!cache) mCount = 0;  // allocation failed: degrade to empty, fail safe
}

NativeCompat& NativeCompat::Get() {
    static NativeCompat instance;
    return instance;
}

const NativeCompatFlag* NativeCompat::Find(const char* name) const {
    if (!name) return nullptr;
    for (std::size_t i = 0; i < kFlagCount; ++i)
        if (std::strcmp(kFlags[i].name, name) == 0) return &kFlags[i];
    return nullptr;
}

NativeCompatTable NativeCompat::Table() const {
    return NativeCompatTable{kFlags, kFlagCount};
}

bool NativeCompat::OptOutActive(const char* name) const {
    if (name) {
        for (std::size_t i = 0; i < kFlagCount; ++i) {
            if (std::strcmp(kFlags[i].name, name) == 0)
                return mTriggered && i < mCount ? (mTriggered[i] == 0)
                                                : true;
        }
    }
    // Unregistered flag — the census `check` gate keeps this impossible. Fail safe
    // to feature-ENABLED (default-ON), matching an unset opt-out env.
    return true;
}

bool NativeCompat::ProbeActive(const char* name) const {
#if !MILO_COMPAT_PROBES
    (void)name;
    return false;  // probes compiled out of release builds
#else
    if (name) {
        for (std::size_t i = 0; i < kFlagCount; ++i) {
            if (std::strcmp(kFlags[i].name, name) == 0)
                return mTriggered && i < mCount && mTriggered[i] != 0;
        }
    }
    return false;
#endif
}
