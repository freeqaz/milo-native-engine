#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Look profile: the one switch between the original hardware's look and the
// native port's polish (DC3 only).
//
//   DC3_LOOK=enhanced   (default, also when unset) native polish ON
//   DC3_LOOK=retail     the original's behaviour, for parity measurement
//                       against Xenia / retail footage
//
// Any other value warns once and falls back to enhanced. The variable is read
// ONCE (first call to Look()), registered in NativeCompatFlags.classification.json.
//
// Only the DC3 retail model (rndshape::kDC3RetailModel) reads it. rb3-xenon
// (DC3 shape header, not the retail model) and rb3 (RB3Wii shape,
// kDC3RetailModel = false) get LookProfile::Native and never read the variable.
// With Native, neither Retail() nor Enhanced() is true, so an enhancement
// written as `if (Look().Enhanced())` and a faithful branch written as
// `if (Look().Retail())` both leave those consumers exactly as they render
// today. Keep it that way: never key an rb3-visible change on
// `!Look().Retail()`.
//
// Hanging an enhancement here: add a bool to LookSettings, set it in
// LookProfile.cpp from the profile (Enhanced -> on), document it below with the
// original behaviour it departs from, and read it at the draw site. One switch,
// many enhancements; do not add a per-feature env var. The exceptions are COST
// knobs, not looks: MILO_RENDER_SCALE (renderScaleExplicit below), so a device
// that cannot carry supersampling keeps the rest of the look, and
// MILO_OUTPUT_MAX_ROWS (outputMaxRowsExplicit), the output resolution's cap.
// ─────────────────────────────────────────────────────────────────────────────

namespace milo {

enum class LookProfile : unsigned char {
    Native,    // consumer has no look switch (rb3, rb3-xenon): unchanged renderer
    Retail,    // DC3: the original hardware's behaviour
    Enhanced,  // DC3: native polish (default)
};

struct LookSettings {
    LookProfile profile = LookProfile::Native;

    bool Retail() const { return profile == LookProfile::Retail; }
    bool Enhanced() const { return profile == LookProfile::Enhanced; }

    // ---- texture sampling (sharpness lane) -----------------------------------
    // Levels of a regular bitmap texture uploaded, base included (0 = the whole
    // authored chain). Retail and Native: the consumer's original rule,
    // rndshape::kBitmapMaxMipLevels (DC3's DxTex::SyncBitmap uploads
    // min(NumMips()+1, 2); rb3-xenon's 0 = all). Enhanced: see SettingsFor.
    int bitmapMipLevels = 0;
    // Sampler max anisotropy for material textures (diffuse/normal/spec/...).
    // The original sets none (fetch-constant aniso_filter 0); Enhanced raises it.
    unsigned short maxAnisotropy = 1;
    // Text and HUD-overlay textures: highest level sampled, base included
    // (0 = no limit). Enhanced uploads whole chains, so this keeps glyphs at
    // the original's min(NumMips()+1, 2) -- see MaterialSetup.cpp.
    int uiMipLevels = 0;
    // LOD bias added to material texture samples (standard shader kLodBias).
    // The original's fetch constants carry lod_bias 0.
    float lodBias = 0.0f;
    // Cube maps upload their whole authored mip chain, as DC3's
    // DxCubeTex::Sync creates them (numMips = mNumMips + 1). Native's one
    // level was a port shortcut; both DC3 profiles fix it, Native keeps it.
    bool cubeMipChain = false;

    // ---- supersampling ---------------------------------------------------------
    // Internal render scale: the frame is drawn at this times its output size
    // and box-filtered back down where it leaves the renderer (present,
    // readback; GpuDeviceDesc::renderScale). 1 = off, the original's one
    // sample per pixel (its world draws were not multisampled either; native
    // keeps its 4x MSAA under every profile). Desktop only (the web build
    // ignores it). Per adapter class, because the cost is the main pass's fill
    // times scale^2: renderScale on a discrete GPU, renderScaleIntegrated on
    // anything else (integrated: a handheld's main pass is ~2 ms at 1x).
    // These are DEFAULTS, and a default applies only when the engine presents
    // to a window: a headless run (tests, gates, screenshot / readback-only
    // captures) draws at 1x unless it asks (renderScaleExplicit). The default
    // is also capped so the drawn frame stays within 3840x2160
    // (ResolveRenderScale, gfx/GpuDevice.h).
    float renderScale = 1.0f;
    float renderScaleIntegrated = 1.0f;
    // MILO_RENDER_SCALE=<float>: an explicit scale, under either DC3 profile,
    // headless included, any adapter class, not capped; clamped to [1, 2].
    // 0 = unset (the profile's default above). It keeps the rest of the look:
    // MILO_RENDER_SCALE=1 is the enhanced look without supersampling (a weak
    // discrete card), 1.5 or 2 tries supersampling on a handheld. Read once
    // with DC3_LOOK; DC3 retail model only -- rb3 / rb3-xenon never read it.
    float renderScaleExplicit = 0.0f;

    // ---- output resolution -------------------------------------------------------
    // The frame is drawn at its output's own resolution: the window's size (or
    // headless MILO_WIDTH x MILO_HEIGHT) fitted to 16:9, with UI, viewports and
    // the post chain's measures laid out in the 1280x720 virtual resolution
    // (GpuDeviceDesc::outputFollowsTarget; also needs the shape's
    // rndshape::kOutputFollowsTarget). The original drew a 1280x720 frame and
    // the scaler stretched it to the display; Retail keeps that frame, scaled
    // into the window by the present path. Enhanced: on.
    bool outputFollowsTarget = false;
    // Output height cap, in rows, on a non-discrete adapter presenting to a
    // window (GpuDeviceDesc::outputMaxRowsIntegrated; 0 = none): a handheld's
    // iGPU draws at most 1920x1080 and the present path scales that into a
    // larger panel. Enhanced: 1080. Discrete adapters are not capped.
    int outputMaxRowsIntegrated = 0;
    // The second COST knob: MILO_OUTPUT_MAX_ROWS=<rows> overrides the cap in
    // both directions, on any adapter, headless included: 0 = no cap (try
    // 1440 on the handheld with 1440), 720 = a 1280x720 frame anywhere.
    // -1 = unset (the default above). Read once with DC3_LOOK; DC3 retail
    // model only. Meaningless under Retail, whose frame is 1280x720 anyway.
    int outputMaxRowsExplicit = -1;
};

// MILO_RENDER_SCALE's value -> LookSettings::renderScaleExplicit: 0 for unset,
// empty or not a positive number; else clamped to [1, 2]. *bad is set when a
// non-empty value was not understood (the caller warns).
float ParseRenderScaleKnob(const char *value, bool *bad = nullptr);

// MILO_OUTPUT_MAX_ROWS's value -> LookSettings::outputMaxRowsExplicit: -1 for
// unset, empty or not a whole number >= 0; else the number (0 = no cap).
// *bad is set when a non-empty value was not understood (the caller warns).
int ParseOutputRowsKnob(const char *value, bool *bad = nullptr);

// The process-wide settings, resolved once.
const LookSettings &Look();

const char *LookProfileName(LookProfile p);

// Tests only: replace the resolved settings for the lifetime of the object
// (nests; restores the previous settings on destruction). Not thread-safe.
class ScopedLookOverride {
public:
    explicit ScopedLookOverride(LookProfile p);
    ~ScopedLookOverride();
    ScopedLookOverride(const ScopedLookOverride &) = delete;
    ScopedLookOverride &operator=(const ScopedLookOverride &) = delete;

private:
    LookSettings mPrev;
};

}  // namespace milo
