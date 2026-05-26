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

// Create GPU texture from Milo bitmap data (handles byte-swap, untile, format conversion)
wgpu::Texture CreateFromBitmap(GpuDevice& gpu, const RndBitmap& bmp, int numMips = 0);

// Create cube texture from 6 face bitmaps (for environment maps)
wgpu::Texture CreateCubeFromBitmaps(GpuDevice& gpu, const RndBitmap* const* faces, int numFaces = 6);

// Create render target texture
wgpu::Texture CreateRenderTarget(GpuDevice& gpu, int w, int h, wgpu::TextureFormat fmt);

// Create depth-stencil target
wgpu::Texture CreateDepthTarget(GpuDevice& gpu, int w, int h);

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
