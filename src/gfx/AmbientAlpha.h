// The ambient constant's alpha (retail standard shader constant c1.w).
//
// Decomp-agnostic: plain integers and floats, the CPU half of a contract the
// standard WGSL (standard_wgsl.inc, retailC1Alpha) mirrors.
//
// Retail RB3 X360, NgMat::SetupAmbient (band.exe TU5 fn_824A9090): +0x18
// tests the material's use_environ; when set, +0x24..+0x58 read
// RndEnviron::Current's ambient colour, ALPHA INCLUDED, and +0xA4 / +0xD0 load
// it into vertex and pixel constant c1; when clear, c1 = (1, 1, 1, 1). Every
// shipped standard program that is not pseudo-HDR carries c1.w into the
// output alpha (decoded from rb3 xbox_shaders, xenia-gpu-shader-compiler):
//   standard_10    VS `mul o2, c1, c0`               PS `mul oC0, r0, r2`
//   standard_110   VS `mul r1, r1, c1; mul o2, r1, c0` (vertex colour x c1 x c0)
//   standard_20010 VS `maxs r0._, c1.ww; muls_prev o2.___w, c0.w`
//                  PS `mul oC0.___w, r0.wwww, r2.wwww`
// so oC0.w = texel.a * c0.w * c1.w (x vertex alpha when prelit). A pseudo-HDR
// program (standard_400010) writes luma to oC0.w instead, and so does the
// engine (bloomMaskScale), so c1.w drops out there as it does on retail.
//
// The engine feeds c1 the way retail does: the scene block's ambientColor is
// the current environ's ambient (the environ is part of the scene block's key,
// WgpuRnd::EnsureSceneUniformsCurrent), so its w carries the environ's
// ambient alpha, and the material says whether its c1 is that ambient or 1.
// Only the RB3 X360 retail light model (rndshape::kRB3RetailLight,
// retailLighting 1) writes a w other than 1 or sets the material bit; DC3
// (retailLighting 2) and RB3-Wii (retailLighting 1, w = 1, bit never set)
// read c1.w = 1, which multiplies exactly.

#pragma once

#include <cstdint>

#include "gfx/ColorModW.h"

// The colorMod.w flag that says this material's c1 is the current environ's
// ambient (retail use_environ), not (1, 1, 1, 1). Bit 12, allocated in
// gfx/ColorModW.h with every other colorMod.w field.
constexpr uint32_t kMatAmbientIsEnvironBit = kColorModWAmbientIsEnvironBit;

// c1.w for a draw: the scene block's ambient alpha when the material takes
// the environ's ambient under the RB3 retail light model, else 1.
inline float RetailC1Alpha(uint32_t colorModW, float retailLighting, float sceneAmbientW) {
    const bool rb3RetailModel = retailLighting > 0.5f && retailLighting < 1.5f;
    return (rb3RetailModel && (colorModW & kMatAmbientIsEnvironBit) != 0u) ? sceneAmbientW
                                                                          : 1.0f;
}
