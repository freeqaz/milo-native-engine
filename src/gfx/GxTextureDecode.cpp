#include "gfx/GxTextureDecode.h"

#include <cstring>

namespace GxTextureDecode {

namespace {

inline uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

inline void Expand565(uint16_t c, uint8_t* rgb) {
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

// One 4x4 CMPR sub-block (8 bytes) into `dst` at (x0, y0) of a w x h RGBA8
// image; texels outside the image (tile padding) are dropped.
void DecodeCmprSubBlock(const uint8_t* blk, uint8_t* dst, int w, int h, int x0, int y0) {
    const uint16_t c0 = BE16(blk), c1 = BE16(blk + 2);
    uint8_t pal[4][4];
    Expand565(c0, pal[0]);
    Expand565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 0xFF;
    for (int ch = 0; ch < 3; ch++) {
        if (c0 > c1) {
            pal[2][ch] = (uint8_t)((2 * pal[0][ch] + pal[1][ch]) / 3);
            pal[3][ch] = (uint8_t)((pal[0][ch] + 2 * pal[1][ch]) / 3);
        } else {
            pal[2][ch] = (uint8_t)((pal[0][ch] + pal[1][ch]) / 2);
            // The transparent entry keeps the midpoint colour so filtering
            // across its edge does not pull in black.
            pal[3][ch] = pal[2][ch];
        }
    }
    pal[2][3] = 0xFF;
    pal[3][3] = c0 > c1 ? 0xFF : 0x00;

    for (int py = 0; py < 4; py++) {
        const int y = y0 + py;
        const uint8_t row = blk[4 + py];
        if (y >= h) continue;
        for (int px = 0; px < 4; px++) {
            const int x = x0 + px;
            if (x >= w) continue;
            std::memcpy(dst + ((size_t)y * w + x) * 4, pal[(row >> (6 - 2 * px)) & 3], 4);
        }
    }
}

// A whole CMPR image: 8x8 tiles, row-major, each TL/TR/BL/BR sub-blocks.
void DecodeCmpr(const uint8_t* src, uint8_t* dst, int w, int h) {
    const int pw = (w + 7) & ~7, ph = (h + 7) & ~7;
    for (int ty = 0; ty < ph; ty += 8) {
        for (int tx = 0; tx < pw; tx += 8) {
            for (int sb = 0; sb < 4; sb++) {
                DecodeCmprSubBlock(src, dst, w, h, tx + (sb & 1) * 4, ty + (sb >> 1) * 4);
                src += 8;
            }
        }
    }
}

int CmprBytes(int w, int h) { return ((w + 7) & ~7) * ((h + 7) & ~7) / 2; }

} // namespace

bool DecodeToRGBA(const uint8_t* src, int pixelBytes, int w, int h, int bpp,
                  unsigned order, std::vector<uint8_t>& dst) {
    if (!src || w <= 0 || h <= 0 || !(order & kGxOrderBit)) return false;
    dst.assign((size_t)w * h * 4, 0);
    const unsigned dxt = order & 0x38;

    if (dxt == 0x08 && bpp == 4) {
        if (pixelBytes < CmprBytes(w, h)) return false;
        DecodeCmpr(src, dst.data(), w, h);
        return true;
    }

    if (dxt == 0x08 && bpp == 8 && (order & 0x100)) {
        const int plane = CmprBytes(w, h);
        if (pixelBytes < 2 * plane) return false;
        DecodeCmpr(src, dst.data(), w, h);
        std::vector<uint8_t> alpha((size_t)w * h * 4);
        DecodeCmpr(src + plane, alpha.data(), w, h);
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) dst[i * 4 + 3] = alpha[i * 4 + 1];
        return true;
    }

    if (dxt == 0 && bpp == 32) {
        const int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
        if (pixelBytes < pw * ph * 4) return false;
        for (int ty = 0; ty < ph; ty += 4) {
            for (int tx = 0; tx < pw; tx += 4) {
                for (int i = 0; i < 16; i++) {
                    const int x = tx + (i & 3), y = ty + (i >> 2);
                    if (x < w && y < h) {
                        uint8_t* d = dst.data() + ((size_t)y * w + x) * 4;
                        d[0] = src[i * 2 + 1];
                        d[1] = src[32 + i * 2];
                        d[2] = src[32 + i * 2 + 1];
                        d[3] = src[i * 2];
                    }
                }
                src += 64;
            }
        }
        return true;
    }

    if (dxt == 0 && bpp == 8 && (order & 0x80)) {
        const int pw = (w + 7) & ~7, ph = (h + 3) & ~3;
        if (pixelBytes < pw * ph) return false;
        for (int ty = 0; ty < ph; ty += 4) {
            for (int tx = 0; tx < pw; tx += 8) {
                for (int i = 0; i < 32; i++) {
                    const int x = tx + (i & 7), y = ty + (i >> 3);
                    if (x < w && y < h) {
                        uint8_t* d = dst.data() + ((size_t)y * w + x) * 4;
                        d[0] = d[1] = d[2] = 0xFF;
                        d[3] = src[i];
                    }
                }
                src += 32;
            }
        }
        return true;
    }

    return false;
}

void DownsampleRGBA(const uint8_t* src, int w, int h, std::vector<uint8_t>& dst) {
    const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    dst.assign((size_t)dw * dh * 4, 0);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            unsigned rgb[3] = {0, 0, 0}, a = 0, plain[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++) {
                const int sx = (x * 2 + (k & 1)) < w ? x * 2 + (k & 1) : w - 1;
                const int sy = (y * 2 + (k >> 1)) < h ? y * 2 + (k >> 1) : h - 1;
                const uint8_t* s = src + ((size_t)sy * w + sx) * 4;
                for (int c = 0; c < 3; c++) {
                    rgb[c] += s[c] * s[3];
                    plain[c] += s[c];
                }
                a += s[3];
            }
            uint8_t* d = dst.data() + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 3; c++)
                d[c] = (uint8_t)(a ? (rgb[c] + a / 2) / a : (plain[c] + 2) / 4);
            d[3] = (uint8_t)((a + 2) / 4);
        }
    }
}

} // namespace GxTextureDecode
