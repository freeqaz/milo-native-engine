#pragma once
// gfx/Canvas2D.h -- a batched 2D canvas for natively drawn UI (dc3 GPU backend
// flavor: WgpuRnd). One primitive, the quad: a rectangle with a UV rectangle,
// four corner colours, an optional texture, rounded corners, an optional ring
// and a soft edge. Everything a UI draws (panels, shadows, glyphs, album art)
// is that quad with different fields set.
//
// WHERE IT MAY BE USED
//   Inside a render pass of WgpuRnd -- anywhere Rnd::DrawRect works, e.g. a
//   UIPanel's Draw(). Begin() returns false outside a pass (and with no GPU
//   device); every other call is then a no-op, so callers need no second check.
//
//       Canvas2D* c = TheCanvas2D();
//       if (c && c->Begin(1280.0f, 720.0f)) {
//           c->FillRect({40, 40, 400, 96}, {0.1f, 0.1f, 0.12f, 0.9f}, 12.0f);
//           c->PushClip({40, 40, 400, 96});
//           Canvas2DQuad g;                       // one glyph from an atlas
//           g.rect = {56, 60, 18, 24};
//           g.u0 = 0.25f; g.v0 = 0.5f; g.u1 = 0.28f; g.v1 = 0.55f;
//           g.tex = fontAtlas; g.texMode = Canvas2DTexMode::kCoverageA;
//           g.SetColor({1, 1, 1, 1});
//           c->Quad(g);
//           c->PopClip();
//           c->End();
//       }
//
// COORDINATES
//   Logical units chosen by the caller in Begin(logicalW, logicalH): origin top
//   left, x right, y down. The logical rectangle (0,0)-(logicalW,logicalH) is
//   mapped onto the pass's CURRENT VIEWPORT, the same mapping Rnd::DrawRect
//   gives a rect in Rnd::Width() x Rnd::Height() pixels. So Begin(1280, 720)
//   covers the game's frame at any window size, render scale or target size.
//   Radius, border and softness are logical too; they are converted to device
//   pixels with the smaller of the two axis scales, and the edge is evaluated
//   by a signed-distance function per device pixel, so it stays sharp at any
//   scale. A quad whose edges land on device-pixel boundaries covers exactly
//   those pixels (no blur) with the default softness.
//
// COLOUR AND BLENDING
//   Colours are straight (non-premultiplied) alpha, in the same encoding
//   Rnd::DrawRect's colours are in (written to the target as given). Blend is
//   normal alpha: rgb = src.rgb*src.a + dst.rgb*(1-src.a); the target's alpha
//   gets src.a + dst.a*(1-src.a), except in the world pass of a frame whose
//   alpha is the bloom mask, where alpha is left alone (as Rnd::DrawRect with
//   no material does). The four corner colours are interpolated bilinearly.
//
// TEXTURES
//   A quad's `tex` is the game's RndTex* (null: untextured). It is resolved to
//   its GPU texture the way a material's diffuse map is (uploaded on first
//   use) when the batch is flushed, so IT MUST STAY ALIVE UNTIL End(). Sampled
//   bilinear, with mips if it has them, clamped to the texels of the quad's own
//   UV rectangle: a sub-rectangle of an atlas never bleeds its neighbours, and
//   UVs do not wrap. u0,v0 is the texture coordinate at the quad's top-left
//   corner and u1,v1 at its bottom-right (v down, 0..1 across the texture).
//     kColor       texel RGBA x colour (album art, icons, pre-coloured art,
//                  and a font atlas whose RGB is part of the glyph).
//     kCoverageA   the texel's ALPHA is coverage, the colour comes from the
//                  quad alone: out = (colour.rgb, colour.a * texel.a). The
//                  texel's RGB is ignored.
//     kCoverageR/G/B  the same from another channel (a single-channel atlas
//                  uploaded as red, a glyph cache, a mask).
//   The game's font atlases (RndFont pages), measured on the 12 atlases
//   embedded in DC3's ui/resource/fonts/gen/*.milo_xbox: all DXT5 (8 bpp,
//   order 0x18; 256x256 or 512x512, with mips), glyph coverage in ALPHA. Their
//   RGB is white in some (default, dev, money) and part of the glyph in others
//   (grey in the shadowed and number fonts, coloured in icons and
//   button_icon). So kCoverageA draws any of them as a flat tint, and kColor
//   draws them as the game does (atlas RGB x colour, atlas alpha as coverage)
//   -- for a white atlas the two give the same pixels.
//
// ORDER, BATCHING AND FLUSHING
//   Quads are drawn in submission order; later quads are on top. Consecutive
//   quads with the same texture and the same clip rectangle share one draw
//   call; a change of either starts a new one, so keep an atlas per run of
//   text and avoid interleaving textures. Quads are held in a per-frame
//   instance arena (128 bytes each) and recorded into the pass when the canvas
//   is FLUSHED, which happens:
//     - at End();
//     - before any other draw the renderer issues in the pass (a mesh, a
//       particle system, Rnd::DrawRect -- so the game's own draws and the
//       canvas interleave in call order), and before the renderer changes the
//       pass's viewport or its clip-plane scissor;
//     - when the pass ends. That also closes the canvas: a session does not
//       outlive its pass, Begin() again in the next one.
//   After a flush the pass is as the renderer expects it: scene bind group in
//   slot 0, the renderer's own scissor (its clip-plane rectangle, or the whole
//   target), viewport untouched. The canvas never changes the viewport.
//   Quads whose padded rectangle lies wholly outside the current clip are
//   dropped on the CPU (a scrolled list costs only its visible rows).
//
// CLIPPING
//   PushClip(rect) intersects `rect` (logical) with the current clip and makes
//   the result current; PopClip() restores the previous one. The stack starts
//   at the renderer's scissor at Begin() (the whole target, or its clip-plane
//   rectangle) and is emptied by End(). Clip edges are rounded to the nearest
//   device pixel (a scissor rectangle; it is not antialiased or rounded).
//
// COST
//   One pipeline per pass kind (format x sample count x depth), compiled when
//   the device comes up (WgpuRnd::PrewarmFixedKeyPipelines) for the overlay,
//   world and texture passes, so the first canvas frame compiles nothing. One
//   Queue::WriteBuffer per flush, one bind group lookup and one draw per batch.
//   Stats() counts them per frame. Nothing here has been timed on a real UI
//   yet; the unit tests hold the draw-call counts, not milliseconds.
//
// The CPU half (quad -> instance record, clip intersection, batch breaks) is
// gfx/Canvas2DBatch.h and has no GPU dependency; tests drive it directly.

#include <cstdint>

class RndTex;

struct Canvas2DRect {
    float x, y, w, h;
};

struct Canvas2DColor {
    float r, g, b, a;
};

enum class Canvas2DTexMode : uint8_t {
    kColor = 0,     // texel RGBA x colour
    kCoverageR = 1, // texel channel is coverage; colour from the quad
    kCoverageG = 2,
    kCoverageB = 3,
    kCoverageA = 4, // a font atlas as a flat tint (see TEXTURES)
};

struct Canvas2DQuad {
    Canvas2DRect rect = {0, 0, 0, 0};       // logical; w or h <= 0 draws nothing
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;   // UV at top-left / bottom-right
    Canvas2DColor tl = {1, 1, 1, 1};        // corner colours, straight alpha
    Canvas2DColor tr = {1, 1, 1, 1};
    Canvas2DColor bl = {1, 1, 1, 1};
    Canvas2DColor br = {1, 1, 1, 1};
    RndTex* tex = nullptr;                  // null: untextured
    Canvas2DTexMode texMode = Canvas2DTexMode::kColor;
    float radius = 0;    // corner radius, logical; clamped to half the short side
    float border = 0;    // > 0: a ring this wide inside the edge, not a fill
    float softness = -1; // edge falloff width, logical; < 0: one device pixel
                         // (antialiasing); 0: hard; large: shadow / glow. The
                         // falloff is centred on the edge and the quad's
                         // geometry grows by half of it to hold the outside.

    void SetColor(const Canvas2DColor& c) { tl = tr = bl = br = c; }
    void SetVerticalGradient(const Canvas2DColor& top, const Canvas2DColor& bottom) {
        tl = tr = top;
        bl = br = bottom;
    }
    void SetHorizontalGradient(const Canvas2DColor& left, const Canvas2DColor& right) {
        tl = bl = left;
        tr = br = right;
    }
};

class Canvas2D {
public:
    // Per-frame counters (reset at the first Begin() of a frame).
    struct FrameStats {
        uint32_t quads = 0;     // quads accepted (not culled, not degenerate)
        uint32_t culled = 0;    // quads dropped as wholly outside the clip
        uint32_t drawCalls = 0; // draws recorded into passes
        uint32_t flushes = 0;   // flushes that recorded at least one draw
    };

    Canvas2D();
    ~Canvas2D();
    Canvas2D(const Canvas2D&) = delete;
    Canvas2D& operator=(const Canvas2D&) = delete;

    // Opens the canvas on the renderer's current pass and viewport. False (and
    // the canvas stays closed) outside a pass, with no device, or for a
    // non-positive size. Begin() on an open canvas ends the open session first.
    bool Begin(float logicalW, float logicalH);
    // Flushes, empties the clip stack and closes the canvas.
    void End();
    bool IsOpen() const { return mOpen; }

    // Adds a quad on top of everything submitted so far.
    void Quad(const Canvas2DQuad& q);
    // A solid (optionally rounded) rectangle: Quad() with one colour.
    void FillRect(const Canvas2DRect& r, const Canvas2DColor& c, float radius = 0.0f);

    void PushClip(const Canvas2DRect& r);
    void PopClip();

    // Records the pending quads into the pass now. Normally the renderer calls
    // it (see ORDER above); call it yourself only before issuing wgpu draws of
    // your own on WgpuRnd's pass encoder without going through CurrentPass().
    void Flush();
    bool HasPending() const { return mPending; }

    const FrameStats& Stats() const { return mStats; }

    // ---- renderer side (WgpuRnd) ----
    // Compiles the pipeline for a pass of this kind now; returns pipelines
    // created (0 when it already exists). `format` is a wgpu::TextureFormat.
    int Warm(uint32_t format, bool hasDepth, bool toTexture, uint32_t samples);
    // The pass is about to end: flush and close.
    void OnPassEnd();
    // The renderer changed the pass's viewport (after flushing): later quads
    // map onto the new one.
    void OnViewportChanged();
    void Terminate();

private:
    struct Impl;
    Impl* mImpl;
    FrameStats mStats;
    bool mOpen = false;
    bool mPending = false;
    bool mFlushing = false;
};

// The renderer's canvas; null when no WgpuRnd exists (a build with no GPU
// backend, or before the renderer is created).
Canvas2D* TheCanvas2D();
