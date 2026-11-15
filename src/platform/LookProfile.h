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
// many enhancements; do not add a per-feature env var.
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

    // ---- enhancements (each true only under Enhanced) ----------------------
    // (none yet: the menu-text lane adds alpha-cut coverage, the sharpness
    // lane texture filtering / supersampling)

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
