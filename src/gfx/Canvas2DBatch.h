#pragma once
// gfx/Canvas2DBatch.h -- the CPU half of gfx/Canvas2D.h: quad -> GPU instance
// record, clip-rectangle stack, batch breaks. No GPU, renderer or game-header
// dependency, so tests drive it without a device. Canvas2D.cpp owns one and
// turns its batches into draws.

#include "gfx/Canvas2D.h"

#include <cstdint>
#include <vector>

// One quad as the vertex buffer holds it (wgpu::VertexStepMode::Instance; the
// vertex shader makes the six corners from the vertex index). 32 floats.
// Positions are in pixels of the pass's viewport, origin at the viewport's top
// left, y down.
struct Canvas2DInstance {
    float cx, cy, hx, hy;   // rect centre and half size, viewport px
    float u0, v0, u1, v1;   // UV at the rect's top-left / bottom-right
    float tl[4], tr[4], bl[4], br[4];
    float radius, border, softness; // device px
    float mode;                     // Canvas2DTexMode as a float
    float ndcX, ndcY;               // 2 / viewport width, 2 / viewport height
    float pad;                      // px the drawn quad grows by on each side
    float reserved;
};
static_assert(sizeof(Canvas2DInstance) == 32 * sizeof(float), "the pipeline's vertex layout");

// A scissor rectangle in pixels of the pass's target.
struct Canvas2DScissor {
    int x, y, w, h;
    bool Empty() const { return w <= 0 || h <= 0; }
    bool operator==(const Canvas2DScissor& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    bool operator!=(const Canvas2DScissor& o) const { return !(*this == o); }
};

// One draw: instances [first, first + count) with one texture and one scissor.
struct Canvas2DBatch {
    RndTex* tex;
    Canvas2DScissor scissor;
    uint32_t first;
    uint32_t count;
};

// What a session draws onto: the pass's viewport and target, in target pixels,
// and the scissor the renderer has set (the whole target when it has none).
struct Canvas2DTarget {
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    int targetW = 0, targetH = 0;
    Canvas2DScissor baseClip = {0, 0, 0, 0};
};

class Canvas2DBuilder {
public:
    // The clip stack is reset to `target.baseClip` (clamped to the target).
    void Begin(float logicalW, float logicalH, const Canvas2DTarget& target);
    // Empties the clip stack and closes; pending instances/batches are kept
    // until ClearPending().
    void End();
    bool IsOpen() const { return mOpen; }
    // A new viewport for later quads; the logical size and clip stack stay.
    // (Clip rectangles already pushed keep their pixels.)
    void SetTarget(const Canvas2DTarget& target);

    // Returns true when the quad was added, false when dropped (closed canvas,
    // non-positive size, empty clip, or wholly outside the clip: *culled).
    bool Quad(const Canvas2DQuad& q, bool* culled = nullptr);
    void PushClip(const Canvas2DRect& logical);
    void PopClip();

    const std::vector<Canvas2DInstance>& Instances() const { return mInstances; }
    const std::vector<Canvas2DBatch>& Batches() const { return mBatches; }
    // Drops the pending instances and batches (after they were drawn). The
    // session, its mapping and its clip stack are untouched.
    void ClearPending();

    // The current clip rectangle, target pixels.
    Canvas2DScissor CurrentClip() const;
    size_t ClipDepth() const { return mClips.size(); }
    // A logical rectangle as a scissor: edges rounded to the nearest device
    // pixel, clamped to the target. Not intersected with the clip stack.
    Canvas2DScissor ToScissor(const Canvas2DRect& logical) const;
    // Device pixels per logical unit.
    float ScaleX() const { return mSx; }
    float ScaleY() const { return mSy; }

    static Canvas2DScissor Intersect(const Canvas2DScissor& a, const Canvas2DScissor& b);
    // Corner `i` (0..5: TL BL TR, TR BL BR) of an instance in normalised device
    // coordinates of the viewport, exactly as the vertex shader places it
    // (padding included). For tests and for readers of the shader.
    static void CornerNdc(const Canvas2DInstance& inst, int i, float out[2]);

private:
    std::vector<Canvas2DInstance> mInstances;
    std::vector<Canvas2DBatch> mBatches;
    std::vector<Canvas2DScissor> mClips; // [0] is the base clip
    Canvas2DTarget mTarget;
    float mLogicalW = 0, mLogicalH = 0;
    float mSx = 1, mSy = 1;
    bool mOpen = false;
};
