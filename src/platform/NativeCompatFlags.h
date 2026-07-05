#pragma once
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// NativeCompat flag registry (native-port-only).
//
// The native/web port scattered ~229 `getenv()` reads across the shared engine
// (`milo-native-engine/src`) and the rb3 glue (`rb3/native/src`): archaeological
// debug probes, shipped default-ON rendering/load workarounds, and value knobs.
// None of these exist on Xbox 360 / Wii — the matched build has no getenv compat
// layer (see NativeSettings.h). This module collapses that scatter into ONE typed
// read-once registry that is the single source of truth for a flag's class, its
// legacy env read-semantics, and its owning subsystem.
//
// The table itself is GENERATED — `scripts/analysis/native_compat_census.py gen`
// joins a fresh source scan against the hand-curated classification sidecar
// (`NativeCompatFlags.classification.json`) and emits `NativeCompatFlags.gen.inc`
// (committed). This header + `NativeCompatFlags.cpp` are the runtime that consumes
// it. Regenerating is idempotent; do not hand-edit the `.gen.inc`.
//
// CENTRAL CORRECTNESS HAZARD (see execution/W0.6/PLAN.md §Key-facts-4): the port
// ships TWO distinct env read semantics and they are NOT interchangeable —
//   • Presence: env-var-present (getenv != nullptr) — ANY value incl. "" and "0".
//   • Truthy:   `e && e[0] && e[0] != '0'` — only a non-empty, non-"0" value.
// Resolving a presence-mode flag with truthy logic (or vice-versa) SILENTLY changes
// behaviour at the mismatched site. Each flag therefore carries its own `FlagRead`
// mode (baked into the generated table from the census scan) and the registry
// resolves each flag by ITS declared mode, reproducing the original idiom exactly
// for every input {unset, "", "0", "1", "x"}.
// ─────────────────────────────────────────────────────────────────────────────

enum class FlagClass {
    Probe,       // archaeological debug probe, default-OFF (opt-in)
    Workaround,  // shipped native-port workaround (usually default-ON opt-out)
    Feature,     // a real toggleable port feature
    Perf,        // a performance/tuning knob
    Unknown,     // scanned but not yet classified (NEEDS-CLASSIFICATION)
};

// How the raw env string is interpreted. Preserves the two legacy idioms exactly
// plus a Value mode for numeric/string knobs (atoi/atof sites).
enum class FlagRead {
    Presence,  // getenv(name) != nullptr
    Truthy,    // e && e[0] && e[0] != '0'
    Value,     // caller reads the raw string; registry treats "present" as set
};

struct NativeCompatFlag {
    const char* name;            // "RB3_GAMEWARM_OFF"
    const char* def;             // human default string: "on" (opt-out) / "off" / "240"
    FlagClass   cls;
    FlagRead    read;            // how the raw env string is interpreted
    const char* owner;           // subsystem: "render/lighting", "load/perf", …
    const char* faithfulStatus;  // "n/a" | "not-live: <reason>" | "live" | "probe"
    const char* docAnchor;       // ledger row anchor (== name today)
};

// Probe compile-out mechanism (skeleton only — do NOT mass-apply this wave). When
// probes are compiled out, `ProbeActive()` is unconditionally false, so a release
// build strips the archaeological probe reads entirely.
#ifndef MILO_COMPAT_PROBES
#define MILO_COMPAT_PROBES (defined(DEBUG) || !defined(NDEBUG))
#endif

// Lightweight non-owning view over the generated table (C++17: no std::span).
struct NativeCompatTable {
    const NativeCompatFlag* first;
    std::size_t             count;
    const NativeCompatFlag* begin() const { return first; }
    const NativeCompatFlag* end() const { return first + count; }
    std::size_t             size() const { return count; }
    const NativeCompatFlag& operator[](std::size_t i) const { return first[i]; }
};

// Read-once registry. Mirrors the NativeSettings::Get() house pattern: a
// function-local static singleton whose constructor reads every flag's env ONCE
// and caches the resolved trigger state; all queries hit the cache.
class NativeCompat {
public:
    static NativeCompat& Get();

    // Returns the feature-ENABLED state for an opt-out flag, resolving the raw env
    // by the flag's declared FlagRead mode. Reproduces the legacy idiom exactly:
    //   Presence opt-out: enabled == (getenv(name) == nullptr)
    //   Truthy   opt-out: enabled == !(e && e[0] && e[0] != '0')
    // For an unregistered name (should never happen — the census `check` gate keeps
    // the registry ⊇ every getenv), fails safe to enabled=true (default-ON).
    bool OptOutActive(const char* name) const;

    // Returns whether an opt-IN probe flag is set. Always false when probes are
    // compiled out (MILO_COMPAT_PROBES == 0), so release strips the probe reads.
    bool ProbeActive(const char* name) const;

    const NativeCompatFlag* Find(const char* name) const;
    NativeCompatTable       Table() const;

private:
    NativeCompat();
    NativeCompat(const NativeCompat&) = delete;
    NativeCompat& operator=(const NativeCompat&) = delete;

    // Parallel to the generated table: cached "env triggered" per flag, resolved
    // once in the constructor by each flag's FlagRead mode.
    const unsigned char* mTriggered;  // owns a heap array sized to Table().size()
    std::size_t          mCount;
};
