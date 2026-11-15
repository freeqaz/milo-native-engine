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
// Only the DC3 shape (rndshape::kDC3RetailModel) reads it. rb3-xenon (DC3 shape
// header, not the retail model) gets LookProfile::Native; the rb3 flavor does
// not link this TU at all (MILO_ENGINE_GPU_PLATFORM_SOURCES). With Native,
// neither Retail() nor Enhanced() is true, so an enhancement written as `if (Look().Enhanced())` and a faithful
// branch written as `if (Look().Retail())` both leave those consumers exactly as
// they render today. Keep it that way: never key an rb3-visible change on
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
    float renderScale = 1.0f;
    float renderScaleIntegrated = 1.0f;
};

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
