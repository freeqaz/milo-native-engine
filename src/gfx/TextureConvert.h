#pragma once

#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <cstddef>

class GpuDevice;
class RndBitmap;

// DXT/BC format identifiers extracted from mOrder & 0x38
enum DXTFormat {
    kDXTNone = 0x00,
    kDXT1    = 0x08,  // BC1 — 8 bytes per 4x4 block
    kDXT3    = 0x10,  // BC2 — 16 bytes per 4x4 block
    kDXT5    = 0x18,  // BC3 — 16 bytes per 4x4 block
    kDXN     = 0x20,  // BC5 — 16 bytes per 4x4 block (normal maps, RG only)
};

namespace TextureConvert {

// Create GPU texture from Milo bitmap data (handles byte-swap, untile, format conversion).
// `numMips` is the bitmap's mip count below the base (0 = count the chain).
// `maxLevels` caps the levels created and uploaded, base included (0 = no cap):
// the consumer's texture loader may upload fewer levels than the bitmap carries
// (DC3's DxTex::SyncBitmap uploads at most 2; rndshape::kBitmapMaxMipLevels).
wgpu::Texture CreateFromBitmap(GpuDevice& gpu, const RndBitmap& bmp, int numMips = 0,
                               int maxLevels = 0);

// Tests only: textures CreateFromBitmap makes from now on also carry
// TextureUsage::CopySrc, so a test can read one back (GpuDevice::ReadbackTexture)
// to check what a sampler would see. No shipping path calls it.
void SetBitmapTexturesCopyableForTests(bool on);

// Create cube texture from 6 face bitmaps (for environment maps)
// mipChain: upload every mip level all six faces carry (DC3's DxCubeTex::Sync
// creates mNumMips + 1 levels); false uploads the base only.
wgpu::Texture CreateCubeFromBitmaps(GpuDevice& gpu, const RndBitmap* const* faces, int numFaces = 6,
                                    bool mipChain = false);

// Create render target texture
wgpu::Texture CreateRenderTarget(GpuDevice& gpu, int w, int h, wgpu::TextureFormat fmt,
                                 const char* label = "RndTexTarget");

// Create depth-stencil target
wgpu::Texture CreateDepthTarget(GpuDevice& gpu, int w, int h,
                                const char* label = "RndTexDepth");

// Map Milo bitmap format to WebGPU texture format
wgpu::TextureFormat MapBitmapFormat(const RndBitmap& bmp, bool hasBCSupport);

// === Xbox 360 data conversion ===

// 16-bit byte-swap for Xbox BE DXT data (in-place).
// dxtType: pass kDXT5 to fix alpha endpoint byte order.
void ByteSwapDXT(uint8_t* data, size_t size, int dxtType = 0);

// Untile Milo's custom tiled layout (mOrder & 4) to linear
// Allocates and returns new buffer; caller owns it
uint8_t* UntileMilo(const RndBitmap& bmp);

// CPU DXT decompression fallback (when GPU lacks BC support)
// dst must be w*h*4 bytes (RGBA8)
void DecompressDXT1(const uint8_t* src, uint8_t* dst, int w, int h);
void DecompressDXT3(const uint8_t* src, uint8_t* dst, int w, int h);
void DecompressDXT5(const uint8_t* src, uint8_t* dst, int w, int h);

// Convert BGRA <-> RGBA in-place
void SwapBGRAtoRGBA(uint8_t* data, int w, int h);

} // namespace TextureConvert
