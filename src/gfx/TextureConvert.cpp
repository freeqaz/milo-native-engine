#include "gfx/TextureConvert.h"
#include "gfx/GpuDevice.h"
#include "rndobj/Bitmap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace TextureConvert {

// ============================================================================
// Xbox 360 byte swap — all DXT block data is big-endian 16-bit words
// ============================================================================

void ByteSwapDXT(uint8_t* data, size_t size, int /*dxtType*/) {
    // Swap every pair of bytes (k8in16 pattern) — converts Xbox 360
    // big-endian 16-bit words to little-endian for PC/GPU.
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint8_t tmp = data[i];
        data[i] = data[i + 1];
        data[i + 1] = tmp;
    }
}

// ============================================================================
// Milo custom untiling (mOrder & 4)
// ============================================================================

// Lookup tables from decomp Bitmap.cpp — Milo's proprietary tile layout
static const char bytes02[64] = {
    0x0,  0x4,  0x8,  0xC,  0x10, 0x14, 0x18, 0x1c, 0x2,  0x6,  0xa,  0xe,  0x12,
    0x16, 0x1a, 0x1e, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x22, 0x26,
    0x2a, 0x2e, 0x32, 0x36, 0x3a, 0x3e, 0x11, 0x15, 0x19, 0x1d, 0x1,  0x5,  0x9,
    0xd,  0x13, 0x17, 0x1b, 0x1f, 0x3,  0x7,  0xb,  0xf,  0x31, 0x35, 0x39, 0x3d,
    0x21, 0x25, 0x29, 0x2d, 0x33, 0x37, 0x3b, 0x3f, 0x23, 0x27, 0x2b, 0x2f
};
static const char bytes13[64] = {
    0x10, 0x14, 0x18, 0x1c, 0x0,  0x4,  0x8,  0xc,  0x12, 0x16, 0x1a, 0x1e, 0x2,
    0x6,  0xa,  0xe,  0x30, 0x34, 0x38, 0x3c, 0x20, 0x24, 0x28, 0x2c, 0x32, 0x36,
    0x3a, 0x3e, 0x22, 0x26, 0x2a, 0x2e, 0x1,  0x5,  0x9,  0xd,  0x11, 0x15, 0x19,
    0x1d, 0x3,  0x7,  0xb,  0xf,  0x13, 0x17, 0x1b, 0x1f, 0x21, 0x25, 0x29, 0x2d,
    0x31, 0x35, 0x39, 0x3d, 0x23, 0x27, 0x2b, 0x2f, 0x33, 0x37, 0x3b, 0x3f
};
static const char hbytes02[128] = {
    0x0,  0x8,  0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x2,  0xa,  0x12, 0x1a, 0x22,
    0x2a, 0x32, 0x3a, 0x4,  0xc,  0x14, 0x1c, 0x24, 0x2c, 0x34, 0x3c, 0x6,  0xe,
    0x16, 0x1e, 0x26, 0x2e, 0x36, 0x3e, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70,
    0x78, 0x42, 0x4a, 0x52, 0x5a, 0x62, 0x6a, 0x72, 0x7a, 0x44, 0x4c, 0x54, 0x5c,
    0x64, 0x6c, 0x74, 0x7c, 0x46, 0x4e, 0x56, 0x5e, 0x66, 0x6e, 0x76, 0x7e, 0x21,
    0x29, 0x31, 0x39, 0x1,  0x9,  0x11, 0x19, 0x23, 0x2b, 0x33, 0x3b, 0x3,  0xb,
    0x13, 0x1b, 0x25, 0x2d, 0x35, 0x3d, 0x5,  0xd,  0x15, 0x1d, 0x27, 0x2f, 0x37,
    0x3f, 0x7,  0xf,  0x17, 0x1f, 0x61, 0x69, 0x71, 0x79, 0x41, 0x49, 0x51, 0x59,
    0x63, 0x6b, 0x73, 0x7b, 0x43, 0x4b, 0x53, 0x5b, 0x65, 0x6d, 0x75, 0x7d, 0x45,
    0x4d, 0x55, 0x5d, 0x67, 0x6f, 0x77, 0x7f, 0x47, 0x4f, 0x57, 0x5f
};
static const char hbytes13[128] = {
    0x20, 0x28, 0x30, 0x38, 0x0,  0x8,  0x10, 0x18, 0x22, 0x2a, 0x32, 0x3a, 0x2,
    0xa,  0x12, 0x1a, 0x24, 0x2c, 0x34, 0x3c, 0x4,  0xc,  0x14, 0x1c, 0x26, 0x2e,
    0x36, 0x3e, 0x6,  0xe,  0x16, 0x1e, 0x60, 0x68, 0x70, 0x78, 0x40, 0x48, 0x50,
    0x58, 0x62, 0x6a, 0x72, 0x7a, 0x42, 0x4a, 0x52, 0x5a, 0x64, 0x6c, 0x74, 0x7c,
    0x44, 0x4c, 0x54, 0x5c, 0x66, 0x6e, 0x76, 0x7e, 0x46, 0x4e, 0x56, 0x5e, 0x1,
    0x9,  0x11, 0x19, 0x21, 0x29, 0x31, 0x39, 0x3,  0xb,  0x13, 0x1b, 0x23, 0x2b,
    0x33, 0x3b, 0x5,  0xd,  0x15, 0x1d, 0x25, 0x2d, 0x35, 0x3d, 0x7,  0xf,  0x17,
    0x1f, 0x27, 0x2f, 0x37, 0x3f, 0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x71, 0x79,
    0x43, 0x4b, 0x53, 0x5b, 0x63, 0x6b, 0x73, 0x7b, 0x45, 0x4d, 0x55, 0x5d, 0x65,
    0x6d, 0x75, 0x7d, 0x47, 0x4f, 0x57, 0x5f, 0x67, 0x6f, 0x77, 0x7f
};

// Reproduced from decomp's RndBitmap::PixelOffset — compute tiled byte offset
static int PixelOffsetTiled(int x, int y, int width, int height,
                            int rowBytes, int bpp, bool& nibble) {
    if (bpp == 8) {
        int doubleRowStride = rowBytes * 2;
        const char* lookup = (((y >> 2) % 4) & 1) ? hbytes13 : hbytes02;
        unsigned char lookupOffset = lookup[(y % 4) * 0x10 + (x % 16)];
        if ((int)lookupOffset > 0x1F) {
            lookupOffset = (lookupOffset + doubleRowStride) - 0x20;
        }
        nibble = false;
        return lookupOffset + ((((y >> 1) & 0xFFFFFFFE) * doubleRowStride) +
                               (((x >> 1) * 4) & 0xFFFFFFE0));
    }

    // 4-bit path
    int yQuadMod = (y >> 2) % 4;
    int tiledOffsetX, tiledOffsetY, tiledStride;
    if (width > 0x80 && height > 0x80) {
        tiledOffsetX = (((y - ((y / 128) << 7)) >> 1) & 0xFFFFFFF8) +
                       ((x >> 1) & 0xFFFFFFC0);
        tiledOffsetY = (((x - ((x / 128) << 7)) >> 2) & 0xFFFFFFF8) +
                       ((y >> 2) & 0xFFFFFFE0) + (yQuadMod * 2);
        tiledStride = (((height - ((height / 128) << 7)) & 0xFFFFFFF0) +
                       (width & 0xFFFFFF80)) * 2;
    } else {
        tiledOffsetX = (y >> 1) & 0xFFFFFFF8;
        tiledOffsetY = ((x >> 2) & 0xFFFFFFF8) + (yQuadMod * 2);
        tiledStride = height * 2;
    }
    const char* lookup2 = (yQuadMod & 1) ? hbytes13 : hbytes02;
    unsigned char nibbleOffset = lookup2[((y % 4) << 5) + (x - ((x / 32) << 5))];
    int offsetShifted = (int)nibbleOffset >> 1;
    nibble = nibbleOffset & 1;
    if (offsetShifted > 0x1F) {
        offsetShifted = (offsetShifted + tiledStride) - 0x20;
    }
    return offsetShifted + ((tiledStride * tiledOffsetY) + (tiledOffsetX * 4));
}

uint8_t* UntileMilo(const RndBitmap& bmp) {
    int w = bmp.Width();
    int h = bmp.Height();
    int bpp = bmp.Bpp();
    int rowBytes = bmp.RowBytes();
    const uint8_t* src = bmp.Pixels();

    // For DXT formats, untiling doesn't apply at the pixel level
    // DXT blocks are already in the right order for GPU consumption after byte swap
    if (bmp.Order() & 0x38) {
        // DXT: just copy as-is (byte swap handles endianness)
        int pixelBytes = bmp.PixelBytes();
        uint8_t* dst = new uint8_t[pixelBytes];
        memcpy(dst, src, pixelBytes);
        return dst;
    }

    // Non-DXT tiled formats: untile pixel by pixel
    int dstRowBytes = (w * bpp + 7) / 8;
    int dstSize = dstRowBytes * h;
    uint8_t* dst = new uint8_t[dstSize];
    memset(dst, 0, dstSize);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool nibble = false;
            int srcOffset = PixelOffsetTiled(x, y, w, h, rowBytes, bpp, nibble);

            if (bpp == 4) {
                uint8_t srcByte = src[srcOffset];
                uint8_t val = nibble ? (srcByte >> 4) : (srcByte & 0x0F);
                int dstOff = (y * dstRowBytes) + (x / 2);
                if (x & 1) {
                    dst[dstOff] = (dst[dstOff] & 0x0F) | (val << 4);
                } else {
                    dst[dstOff] = (dst[dstOff] & 0xF0) | val;
                }
            } else if (bpp == 8) {
                dst[y * dstRowBytes + x] = src[srcOffset];
            } else if (bpp == 32) {
                memcpy(&dst[(y * dstRowBytes) + x * 4],
                       &src[srcOffset], 4);
            }
        }
    }
    return dst;
}

// ============================================================================
// DXT CPU decompression fallback
// ============================================================================

static void DecodeRGB565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5) & 0x3F) * 255 / 63;
    b = (c & 0x1F) * 255 / 31;
}

static void DecompressDXT1Block(const uint8_t* block, uint8_t* dst, int dstStride) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);

    uint8_t r[4], g[4], b[4], a[4];
    DecodeRGB565(c0, r[0], g[0], b[0]); a[0] = 255;
    DecodeRGB565(c1, r[1], g[1], b[1]); a[1] = 255;

    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3; a[2] = 255;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3; a[3] = 255;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2; a[2] = 255;
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0; // transparent black
    }

    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            int idx = (indices >> ((py * 4 + px) * 2)) & 3;
            uint8_t* out = dst + py * dstStride + px * 4;
            out[0] = r[idx]; out[1] = g[idx]; out[2] = b[idx]; out[3] = a[idx];
        }
    }
}

void DecompressDXT1(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int dstStride = w * 4;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t blockPixels[4 * 4 * 4]; // 4x4 RGBA
            DecompressDXT1Block(src, blockPixels, 4 * 4);
            src += 8;

            // Copy block to destination
            for (int py = 0; py < 4 && (by * 4 + py) < h; py++) {
                for (int px = 0; px < 4 && (bx * 4 + px) < w; px++) {
                    int dstIdx = ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                    int srcIdx = (py * 4 + px) * 4;
                    memcpy(&dst[dstIdx], &blockPixels[srcIdx], 4);
                }
            }
        }
    }
}

void DecompressDXT3(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            // First 8 bytes: explicit alpha (4 bits per pixel)
            const uint8_t* alphaBlock = src;
            const uint8_t* colorBlock = src + 8;
            src += 16;

            uint8_t blockPixels[4 * 4 * 4];
            DecompressDXT1Block(colorBlock, blockPixels, 4 * 4);

            // Override alpha with explicit values
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int alphaIdx = py * 4 + px;
                    uint8_t alphaByte = alphaBlock[alphaIdx / 2];
                    uint8_t alpha4 = (alphaIdx & 1) ? (alphaByte >> 4) : (alphaByte & 0xF);
                    blockPixels[(py * 4 + px) * 4 + 3] = (alpha4 << 4) | alpha4;
                }
            }

            for (int py = 0; py < 4 && (by * 4 + py) < h; py++) {
                for (int px = 0; px < 4 && (bx * 4 + px) < w; px++) {
                    int dstIdx = ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                    int srcIdx = (py * 4 + px) * 4;
                    memcpy(&dst[dstIdx], &blockPixels[srcIdx], 4);
                }
            }
        }
    }
}

static void DecompressDXT5AlphaBlock(const uint8_t* block, uint8_t alphas[4][4]) {
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];

    uint8_t palette[8];
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        palette[2] = (6 * a0 + 1 * a1) / 7;
        palette[3] = (5 * a0 + 2 * a1) / 7;
        palette[4] = (4 * a0 + 3 * a1) / 7;
        palette[5] = (3 * a0 + 4 * a1) / 7;
        palette[6] = (2 * a0 + 5 * a1) / 7;
        palette[7] = (1 * a0 + 6 * a1) / 7;
    } else {
        palette[2] = (4 * a0 + 1 * a1) / 5;
        palette[3] = (3 * a0 + 2 * a1) / 5;
        palette[4] = (2 * a0 + 3 * a1) / 5;
        palette[5] = (1 * a0 + 4 * a1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }

    // 3-bit indices packed into 6 bytes (48 bits for 16 pixels)
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (uint64_t)block[2 + i] << (i * 8);
    }

    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            int idx = (bits >> ((py * 4 + px) * 3)) & 7;
            alphas[py][px] = palette[idx];
        }
    }
}

void DecompressDXT5(const uint8_t* src, uint8_t* dst, int w, int h) {
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t* alphaBlock = src;
            const uint8_t* colorBlock = src + 8;
            src += 16;

            uint8_t blockPixels[4 * 4 * 4];
            DecompressDXT1Block(colorBlock, blockPixels, 4 * 4);

            uint8_t alphas[4][4];
            DecompressDXT5AlphaBlock(alphaBlock, alphas);

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    blockPixels[(py * 4 + px) * 4 + 3] = alphas[py][px];
                }
            }

            for (int py = 0; py < 4 && (by * 4 + py) < h; py++) {
                for (int px = 0; px < 4 && (bx * 4 + px) < w; px++) {
                    int dstIdx = ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                    int srcIdx = (py * 4 + px) * 4;
                    memcpy(&dst[dstIdx], &blockPixels[srcIdx], 4);
                }
            }
        }
    }
}

void DecompressDXN(const uint8_t* src, uint8_t* dst, int w, int h) {
    // DXN/BC5: two DXT5-alpha blocks per 4x4 tile → R and G channels
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t redValues[4][4];
            uint8_t greenValues[4][4];
            DecompressDXT5AlphaBlock(src, redValues);
            DecompressDXT5AlphaBlock(src + 8, greenValues);
            src += 16;

            for (int py = 0; py < 4 && (by * 4 + py) < h; py++) {
                for (int px = 0; px < 4 && (bx * 4 + px) < w; px++) {
                    int idx = ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                    dst[idx + 0] = redValues[py][px];
                    dst[idx + 1] = greenValues[py][px];
                    dst[idx + 2] = 0xFF; // B channel (unused for normal maps)
                    dst[idx + 3] = 0xFF; // A
                }
            }
        }
    }
}

// ============================================================================
// Channel order conversion
// ============================================================================

void SwapBGRAtoRGBA(uint8_t* data, int w, int h) {
    int count = w * h;
    for (int i = 0; i < count; i++) {
        uint8_t* px = data + i * 4;
        uint8_t tmp = px[0];
        px[0] = px[2];
        px[2] = tmp;
    }
}

// ============================================================================
// Format mapping
// ============================================================================

wgpu::TextureFormat MapBitmapFormat(const RndBitmap& bmp, bool hasBCSupport) {
    unsigned int dxt = bmp.Order() & 0x38;

    if (dxt && hasBCSupport) {
        switch (dxt) {
        case kDXT1: return wgpu::TextureFormat::BC1RGBAUnorm;
        case kDXT3: return wgpu::TextureFormat::BC2RGBAUnorm;
        case kDXT5: return wgpu::TextureFormat::BC3RGBAUnorm;
        // DXN (BC5) always CPU-decompressed to RGBA8. BC5RGUnorm returns
        // (R, G, 0, 1) which breaks the shader's DXT5nm/RGB normal decode
        // heuristic — the zero B channel produces Z = -1 (flipped normal).
        // CPU decompress sets B=255, A=255, giving correct Z = +1.
        case kDXN:  return wgpu::TextureFormat::RGBA8Unorm;
        default:    break; // fall through to RGBA8
        }
    }

    // Non-DXT, BC not supported, or unrecognized DXT variant: decompress to RGBA8
    return wgpu::TextureFormat::RGBA8Unorm;
}

// ============================================================================
// GPU texture creation
// ============================================================================

wgpu::Texture CreateFromBitmap(GpuDevice& gpu, const RndBitmap& bmp, int numMips) {
    int w = bmp.Width();
    int h = bmp.Height();
    if (w == 0 || h == 0 || !bmp.Pixels()) return nullptr;

    unsigned int order = bmp.Order();
    unsigned int dxt = order & 0x38;
    int bpp = bmp.Bpp();
    bool hasBCSupport = gpu.HasBCCompression();
    wgpu::TextureFormat fmt = MapBitmapFormat(bmp, hasBCSupport);

    // Count actual mip levels
    int mipCount = 1;
    if (numMips > 0) {
        mipCount = numMips + 1;
    } else {
        const RndBitmap* mip = bmp.nextMip();
        while (mip) {
            mipCount++;
            mip = mip->nextMip();
        }
    }

    // Create GPU texture
    wgpu::TextureDescriptor texDesc{};
    texDesc.size = {(uint32_t)w, (uint32_t)h, 1};
    texDesc.format = fmt;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texDesc.mipLevelCount = mipCount;
    wgpu::Texture tex = gpu.Device().CreateTexture(&texDesc);
    if (!tex) return nullptr;

    // Upload each mip level
    const RndBitmap* curMip = &bmp;
    for (int mipLevel = 0; mipLevel < mipCount && curMip; mipLevel++) {
        int mw = curMip->Width();
        int mh = curMip->Height();
        int pixelBytes = curMip->PixelBytes();
        const uint8_t* srcPixels = curMip->Pixels();

        if (!srcPixels || pixelBytes == 0) break;

        // Make a working copy for in-place transforms
        std::vector<uint8_t> workBuf(srcPixels, srcPixels + pixelBytes);
        uint8_t* workData = workBuf.data();

        // Step 1: Untile if needed (mOrder & 4)
        uint8_t* untiled = nullptr;
        if (order & 4) {
            untiled = UntileMilo(*curMip);
            workData = untiled;
        }

        // Step 2: Byte-swap DXT data from Xbox BE
        if (dxt) {
            ByteSwapDXT(workData, pixelBytes, dxt);
        }

        // Step 3: Determine upload data and format
        const uint8_t* uploadData = workData;
        size_t uploadSize = pixelBytes;
        std::vector<uint8_t> decompBuf;

        bool needsCpuDecomp = dxt && (fmt == wgpu::TextureFormat::RGBA8Unorm);
        if (needsCpuDecomp) {
            // CPU decompress DXT -> RGBA8 (used when BC not supported, or
            // for DXN which always decompresses to RGBA8 for correct shader decode)
            decompBuf.resize(mw * mh * 4);
            switch (dxt) {
            case kDXT1: DecompressDXT1(workData, decompBuf.data(), mw, mh); break;
            case kDXT3: DecompressDXT3(workData, decompBuf.data(), mw, mh); break;
            case kDXT5: DecompressDXT5(workData, decompBuf.data(), mw, mh); break;
            case kDXN:  DecompressDXN(workData, decompBuf.data(), mw, mh); break;
            }
            uploadData = decompBuf.data();
            uploadSize = decompBuf.size();
        } else if (!dxt) {
            // Non-DXT: handle channel order
            if (bpp == 32) {
                if (!(order & 1)) {
                    // BGRA -> RGBA
                    SwapBGRAtoRGBA(workData, mw, mh);
                }
                uploadData = workData;
                uploadSize = mw * mh * 4;
            } else if (bpp == 24) {
                // Expand RGB24 to RGBA32
                decompBuf.resize(mw * mh * 4);
                for (int i = 0; i < mw * mh; i++) {
                    if (order & 1) {
                        decompBuf[i * 4 + 0] = workData[i * 3 + 0];
                        decompBuf[i * 4 + 1] = workData[i * 3 + 1];
                        decompBuf[i * 4 + 2] = workData[i * 3 + 2];
                    } else {
                        decompBuf[i * 4 + 0] = workData[i * 3 + 2]; // B->R
                        decompBuf[i * 4 + 1] = workData[i * 3 + 1]; // G
                        decompBuf[i * 4 + 2] = workData[i * 3 + 0]; // R->B
                    }
                    decompBuf[i * 4 + 3] = 0xFF;
                }
                uploadData = decompBuf.data();
                uploadSize = decompBuf.size();
            } else if (bpp == 8 || bpp == 4) {
                // Palette-indexed: expand via PixelColor
                decompBuf.resize(mw * mh * 4);
                for (int py = 0; py < mh; py++) {
                    for (int px = 0; px < mw; px++) {
                        uint8_t r, g, b, a;
                        curMip->PixelColor(px, py, r, g, b, a);
                        int idx = (py * mw + px) * 4;
                        decompBuf[idx + 0] = r;
                        decompBuf[idx + 1] = g;
                        decompBuf[idx + 2] = b;
                        decompBuf[idx + 3] = a;
                    }
                }
                uploadData = decompBuf.data();
                uploadSize = decompBuf.size();
            }
        }

        // Step 4: Upload to GPU
        wgpu::TexelCopyTextureInfo dstInfo{};
        dstInfo.texture = tex;
        dstInfo.mipLevel = mipLevel;

        wgpu::TexelCopyBufferLayout srcLayout{};
        if (dxt && !needsCpuDecomp) {
            // BC-compressed: block pitch
            int blockW = (mw + 3) / 4;
            int blockBytes = (dxt == kDXT1) ? 8 : 16;
            srcLayout.bytesPerRow = blockW * blockBytes;
        } else {
            srcLayout.bytesPerRow = mw * 4;
        }

        wgpu::Extent3D extent = {(uint32_t)mw, (uint32_t)mh, 1};
        gpu.Queue().WriteTexture(&dstInfo, uploadData, uploadSize, &srcLayout, &extent);

        delete[] untiled;
        curMip = curMip->nextMip();
    }

    return tex;
}

wgpu::Texture CreateCubeFromBitmaps(GpuDevice& gpu, const RndBitmap* const* faces, int numFaces) {
    // All 6 faces must have the same dimensions
    int w = faces[0]->Width();
    int h = faces[0]->Height();
    if (w == 0 || h == 0) return nullptr;

    bool hasBCSupport = gpu.HasBCCompression();
    wgpu::TextureFormat fmt = MapBitmapFormat(*faces[0], hasBCSupport);

    wgpu::TextureDescriptor texDesc{};
    texDesc.size = {(uint32_t)w, (uint32_t)h, 6};
    texDesc.dimension = wgpu::TextureDimension::e2D;
    texDesc.format = fmt;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texDesc.mipLevelCount = 1;
    texDesc.viewFormatCount = 0;
    wgpu::Texture tex = gpu.Device().CreateTexture(&texDesc);
    if (!tex) return nullptr;

    for (int face = 0; face < numFaces && face < 6; face++) {
        const RndBitmap& bmp = *faces[face];
        int fw = bmp.Width();
        int fh = bmp.Height();
        if (fw == 0 || fh == 0 || !bmp.Pixels()) continue;

        unsigned int order = bmp.Order();
        unsigned int dxt = order & 0x38;
        int bpp = bmp.Bpp();
        int pixelBytes = bmp.PixelBytes();
        const uint8_t* srcPixels = bmp.Pixels();

        // Make working copy
        std::vector<uint8_t> workBuf(srcPixels, srcPixels + pixelBytes);
        uint8_t* workData = workBuf.data();

        // Untile if needed
        uint8_t* untiled = nullptr;
        if (order & 4) {
            untiled = UntileMilo(bmp);
            workData = untiled;
        }

        // Byte-swap DXT
        if (dxt) {
            ByteSwapDXT(workData, pixelBytes, dxt);
        }

        // Determine upload data
        const uint8_t* uploadData = workData;
        size_t uploadSize = pixelBytes;
        std::vector<uint8_t> decompBuf;

        if (dxt && !hasBCSupport) {
            decompBuf.resize(fw * fh * 4);
            switch (dxt) {
            case kDXT1: DecompressDXT1(workData, decompBuf.data(), fw, fh); break;
            case kDXT3: DecompressDXT3(workData, decompBuf.data(), fw, fh); break;
            case kDXT5: DecompressDXT5(workData, decompBuf.data(), fw, fh); break;
            case kDXN:  DecompressDXN(workData, decompBuf.data(), fw, fh); break;
            }
            uploadData = decompBuf.data();
            uploadSize = decompBuf.size();
        } else if (!dxt && bpp == 32) {
            if (!(order & 1)) {
                SwapBGRAtoRGBA(workData, fw, fh);
            }
            uploadSize = fw * fh * 4;
        }

        // Upload to correct array layer
        wgpu::TexelCopyTextureInfo dstInfo{};
        dstInfo.texture = tex;
        dstInfo.mipLevel = 0;
        dstInfo.origin = {0, 0, (uint32_t)face};

        wgpu::TexelCopyBufferLayout srcLayout{};
        if (dxt && hasBCSupport) {
            int blockW = (fw + 3) / 4;
            int blockBytes = (dxt == kDXT1) ? 8 : 16;
            srcLayout.bytesPerRow = blockW * blockBytes;
        } else {
            srcLayout.bytesPerRow = fw * 4;
        }

        wgpu::Extent3D extent = {(uint32_t)fw, (uint32_t)fh, 1};
        gpu.Queue().WriteTexture(&dstInfo, uploadData, uploadSize, &srcLayout, &extent);

        delete[] untiled;
    }

    return tex;
}

wgpu::Texture CreateRenderTarget(GpuDevice& gpu, int w, int h, wgpu::TextureFormat fmt) {
    wgpu::TextureDescriptor desc{};
    desc.size = {(uint32_t)w, (uint32_t)h, 1};
    desc.format = fmt;
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding
               | wgpu::TextureUsage::CopyDst;
    return gpu.Device().CreateTexture(&desc);
}

wgpu::Texture CreateDepthTarget(GpuDevice& gpu, int w, int h) {
    wgpu::TextureDescriptor desc{};
    desc.size = {(uint32_t)w, (uint32_t)h, 1};
    desc.format = wgpu::TextureFormat::Depth24PlusStencil8;
    desc.usage = wgpu::TextureUsage::RenderAttachment;
    return gpu.Device().CreateTexture(&desc);
}

} // namespace TextureConvert
