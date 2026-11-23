// The allocation of MaterialUniforms::colorMod[3] (WGSL material.colorMod.w).
//
// Decomp-agnostic. colorMod.w is one u32 that carries the colour-mod mode and
// every per-material flag the standard shader reads, so the struct stays one
// 256-byte uniform-ring slot (UniformStructs.h). This header is the one place
// a field's bits are allocated: the CPU writers (platform/MaterialSetup.cpp,
// gfx/AmbientAlpha.h) take their bits from here, and the standard WGSL
// (gfx/standard_wgsl.inc, a raw string literal that is also hot-reloaded from
// disk, so it cannot include this) reads each flag in one accessor, with a
// literal shift. tests/test_environ_ambient_alpha.cpp checks every accessor's
// shift against kColorModWFields and that no two fields share a bit.
//
//   bits  0-7   mode: 0 none, 1 AlphaPack, 2 AlphaUnpackModulate, 3 Modulate
//                 (RndMat mColorModFlags; WGSL colorModMode)
//   bit   8     the texel alpha selects a colour-mod band (rndshape::
//                 ColorModAlphaIsBandSelector; WGSL colorModAlphaIsBandSelector)
//   bit   9     DC3 screen-aligned texture coordinates (rndshape::
//                 MatScreenAligned; WGSL materialScreenAligned)
//   bit  10     DC3 flip_normal (WGSL materialFlipNormal)
//   bit  11     fade_out (rndshape::MatFadesOut; WGSL materialFadesOut)
//   bit  12     c1 is the environ's ambient (rndshape::MatAmbientIsEnviron;
//                 WGSL retailC1Alpha)
//   bits 13-15  free
//   bits 16-31  world refraction strength, f16 (WGSL refractStrength)

#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t kColorModWModeMask = 0xFFu;
constexpr uint32_t kColorModWBandSelectorShift = 8;
constexpr uint32_t kColorModWScreenAlignedShift = 9;
constexpr uint32_t kColorModWFlipNormalShift = 10;
constexpr uint32_t kColorModWFadesOutShift = 11;
constexpr uint32_t kColorModWAmbientIsEnvironShift = 12;
constexpr uint32_t kColorModWRefractStrengthShift = 16;
constexpr uint32_t kColorModWRefractStrengthMask = 0xFFFFu << kColorModWRefractStrengthShift;

constexpr uint32_t kColorModWBandSelectorBit = 1u << kColorModWBandSelectorShift;
constexpr uint32_t kColorModWScreenAlignedBit = 1u << kColorModWScreenAlignedShift;
constexpr uint32_t kColorModWFlipNormalBit = 1u << kColorModWFlipNormalShift;
constexpr uint32_t kColorModWFadesOutBit = 1u << kColorModWFadesOutShift;
constexpr uint32_t kColorModWAmbientIsEnvironBit = 1u << kColorModWAmbientIsEnvironShift;

// Every allocated field: its mask, and for a one-bit flag the WGSL accessor
// that reads it (nullptr for the multi-bit fields, read by colorModMode and
// refractStrength). A new field is added here, or the collision check cannot
// see it.
struct ColorModWField {
    const char* name;
    uint32_t mask;
    const char* wgslAccessor;
};
constexpr ColorModWField kColorModWFields[] = {
    {"mode", kColorModWModeMask, nullptr},
    {"band selector", kColorModWBandSelectorBit, "colorModAlphaIsBandSelector"},
    {"screen aligned", kColorModWScreenAlignedBit, "materialScreenAligned"},
    {"flip_normal", kColorModWFlipNormalBit, "materialFlipNormal"},
    {"fade_out", kColorModWFadesOutBit, "materialFadesOut"},
    {"ambient is environ", kColorModWAmbientIsEnvironBit, "retailC1Alpha"},
    {"refraction strength", kColorModWRefractStrengthMask, nullptr},
};

// True when no two fields of kColorModWFields share a bit.
constexpr bool ColorModWFieldsDisjoint() {
    uint32_t seen = 0;
    for (const ColorModWField& f : kColorModWFields) {
        if ((seen & f.mask) != 0u) return false;
        seen |= f.mask;
    }
    return true;
}
static_assert(ColorModWFieldsDisjoint(), "two colorMod.w fields share a bit (gfx/ColorModW.h)");
