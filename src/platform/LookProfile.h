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
// many enhancements; do not add a per-feature env var. The one exception is a
// COST knob, not a look: MILO_RENDER_SCALE (renderScaleExplicit below), so a
// device that cannot carry supersampling keeps the rest of the look.
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

    // ---- post chain (np-gfx-post) ----------------------------------------------
    // np-gfx-post round 10: the post chain's world grade only, i.e. the post
    // run before the HUD and UI draw (WgpuRnd::FlushPostProcessingForOverlay,
    // PostProcPass::SetWorldGrade). A post that runs over the UI (menus graded
    // at EndDrawing) stays retail, so UI, HUD and text never change. Engine
    // doc §37.11 has the measurements and costs.
    //   postPrecision    the bloom sets in RGBA16Float, still saturated at 1
    //                    after every pass as retail's 8-bit EDRAM resolve
    //                    saturates them (A8R8G8B8, NgPostProc::
    //                    BloomTextureSet), and keeping that resolve's dead zone
    //                    (8-bit rounding drops whatever a pass leaves under half
    //                    a level: a dim surface's own glow) as a smooth knee,
    //                    so the steps go and the energy stays; a static sub-LSB
    //                    dither on the composite's 8-bit write. Retail: 8-bit
    //                    sets, no dither.
    //   postBloomFilter  tent (not box) downsample into the bloom sets; the
    //                    streak arm's w/16 set from all 256 texels of its block
    //                    (retail's bloom.ps reads 16 of them, 4 bilinear taps
    //                    at +-2 texels); a cubic B-spline (not bilinear)
    //                    upsample of the plain arm's coarse sets (combined once
    //                    at w/4) and of the glare arm's set.
    //   postDofFilter    depth of field: tent downsample, B-spline upsample of
    //                    the blur in the full-resolution composite, the 8 blur
    //                    taps scaled so the total blur radius (second moment)
    //                    stays retail's. Retail: box, bilinear.
    bool postPrecision = false;
    bool postBloomFilter = false;
    bool postDofFilter = false;
};

// MILO_RENDER_SCALE's value -> LookSettings::renderScaleExplicit: 0 for unset,
// empty or not a positive number; else clamped to [1, 2]. *bad is set when a
// non-empty value was not understood (the caller warns).
float ParseRenderScaleKnob(const char *value, bool *bad = nullptr);

// The process-wide settings, resolved once.
const LookSettings &Look();

const char *LookProfileName(LookProfile p);

// Inspection (dc3's `{rnd native_look retail|enhanced}`): switch between the
// two DC3 profiles at run time, so an A/B takes the same frozen frame. Ignored
// (returns false) for a consumer whose profile is Native.
bool SetLookProfileForInspection(LookProfile p);
// The same for the post chain's fields only (postPrecision, postBloomFilter,
// postDofFilter), the rest of the profile untouched: an A/B of the post look
// without the other enhancements (alpha-cut coverage) changing too.
bool SetLookPostForInspection(bool enhanced);
// One post field at a time (the per-item cost A/B: each enhancement against
// retail in the same run); the fields not named are set retail.
bool SetLookPostFieldsForInspection(bool precision, bool bloomFilter, bool dofFilter);

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
