#pragma once

// GxTextureDecode — CPU decode of RB3-Wii (GameCube/Wii GX) texture layouts to
// linear RGBA8.
//
// RndBitmap marks a GX-native pixel layout with the 0x40 order bit
// (RndTex::PlatformBppOrder on Wii; WiiTex OrderFromFormat maps GX_TF_CMPR to
// 0x48, GX_TF_RGBA8 to 0x40 and GX_TF_I8 to 0xC0). Xbox and DC3 bitmaps never
// set that bit. The layouts decoded here, all measured on retail RB3 Wii data:
//
//   order 0x48,  bpp 4   GX_TF_CMPR: 8x8 tiles in row-major order, each four
//                        DXT1 sub-blocks (TL, TR, BL, BR) with big-endian
//                        RGB565 endpoints and MSB-first 2-bit indices.
//   order 0x148, bpp 8   two CMPR planes of w*h/2 bytes each: colour, then an
//                        alpha plane whose green channel is the alpha. This is
//                        how RB3 Wii stores every translucent texture (RB3 Xbox
//                        uses DXT5 for the same assets; the decoded colour and
//                        alpha agree with it).
//   order 0x40,  bpp 32  GX_TF_RGBA8: 4x4 tiles of 64 bytes, 16 AR pairs then
//                        16 GB pairs (RndBitmap::ConvertColor's 0x40 branch).
//   order 0xC0,  bpp 8   GX_TF_I8: 8x4 tiles; decoded as RndBitmap::ConvertColor
//                        defines an 0xC0 bitmap, white with alpha = I.
//
// Anything else returns false and is left to the caller's existing path.

#include <cstdint>
#include <vector>

namespace GxTextureDecode {

constexpr unsigned kGxOrderBit = 0x40;

// Decode a GX-layout bitmap to w*h RGBA8 (straight alpha). `pixelBytes` is the
// size of the source buffer; a layout that would read past it is rejected.
bool DecodeToRGBA(const uint8_t* src, int pixelBytes, int w, int h, int bpp,
                  unsigned order, std::vector<uint8_t>& dst);

// Halve an RGBA8 image (2x2 box, colour weighted by alpha so transparent texels
// do not darken the edges). Odd sizes clamp; the result is max(1, w/2) x
// max(1, h/2).
void DownsampleRGBA(const uint8_t* src, int w, int h, std::vector<uint8_t>& dst);

} // namespace GxTextureDecode
