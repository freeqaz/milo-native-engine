// gfx/Canvas2DBatch.cpp -- the CPU half of the 2D canvas (see Canvas2DBatch.h).
#include "gfx/Canvas2DBatch.h"

#include <algorithm>
#include <cmath>

namespace {

Canvas2DScissor ClampToTarget(const Canvas2DScissor& s, int tw, int th) {
    int x0 = std::max(s.x, 0), y0 = std::max(s.y, 0);
    int x1 = std::min(s.x + s.w, tw), y1 = std::min(s.y + s.h, th);
    if (x1 <= x0 || y1 <= y0) return {std::min(x0, std::max(tw, 0)), std::min(y0, std::max(th, 0)), 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

void CopyColor(float dst[4], const Canvas2DColor& c) {
    dst[0] = c.r;
    dst[1] = c.g;
    dst[2] = c.b;
    dst[3] = c.a;
}

} // namespace

Canvas2DScissor Canvas2DBuilder::Intersect(const Canvas2DScissor& a, const Canvas2DScissor& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
}

void Canvas2DBuilder::SetTarget(const Canvas2DTarget& target) {
    mTarget = target;
    mSx = mLogicalW > 0 ? target.vpW / mLogicalW : 1.0f;
    mSy = mLogicalH > 0 ? target.vpH / mLogicalH : 1.0f;
}

void Canvas2DBuilder::Begin(float logicalW, float logicalH, const Canvas2DTarget& target) {
    mLogicalW = logicalW;
    mLogicalH = logicalH;
    SetTarget(target);
    mClips.clear();
    mClips.push_back(ClampToTarget(target.baseClip, target.targetW, target.targetH));
    mOpen = true;
}

void Canvas2DBuilder::End() {
    mClips.clear();
    mOpen = false;
}

void Canvas2DBuilder::ClearPending() {
    mInstances.clear();
    mBatches.clear();
}

Canvas2DScissor Canvas2DBuilder::CurrentClip() const {
    if (mClips.empty()) return {0, 0, 0, 0};
    return mClips.back();
}

Canvas2DScissor Canvas2DBuilder::ToScissor(const Canvas2DRect& r) const {
    // Edges to the nearest device-pixel boundary; a rect that rounds to nothing
    // clips everything.
    const int x0 = (int)std::floor(mTarget.vpX + r.x * mSx + 0.5f);
    const int y0 = (int)std::floor(mTarget.vpY + r.y * mSy + 0.5f);
    const int x1 = (int)std::floor(mTarget.vpX + (r.x + r.w) * mSx + 0.5f);
    const int y1 = (int)std::floor(mTarget.vpY + (r.y + r.h) * mSy + 0.5f);
    return ClampToTarget({x0, y0, x1 - x0, y1 - y0}, mTarget.targetW, mTarget.targetH);
}

void Canvas2DBuilder::PushClip(const Canvas2DRect& logical) {
    if (!mOpen) return;
    mClips.push_back(Intersect(CurrentClip(), ToScissor(logical)));
}

void Canvas2DBuilder::PopClip() {
    // The base clip (index 0) is the session's, not the caller's.
    if (!mOpen || mClips.size() <= 1) return;
    mClips.pop_back();
}

bool Canvas2DBuilder::Quad(const Canvas2DQuad& q, bool* culled) {
    if (culled) *culled = false;
    if (!mOpen) return false;
    if (!(q.rect.w > 0.0f) || !(q.rect.h > 0.0f)) return false;
    if (!(mTarget.vpW > 0.0f) || !(mTarget.vpH > 0.0f)) return false;
    const Canvas2DScissor clip = CurrentClip();
    if (clip.Empty()) {
        if (culled) *culled = true;
        return false;
    }

    // Logical -> viewport pixels. Radius, border and softness take the smaller
    // axis scale, so a non-uniform mapping never makes a corner larger than
    // the side it rounds.
    const float k = std::min(mSx, mSy);
    Canvas2DInstance in;
    in.hx = q.rect.w * 0.5f * mSx;
    in.hy = q.rect.h * 0.5f * mSy;
    in.cx = q.rect.x * mSx + in.hx;
    in.cy = q.rect.y * mSy + in.hy;
    in.softness = q.softness < 0.0f ? 1.0f : q.softness * k;
    in.radius = std::min(std::max(q.radius, 0.0f) * k, std::min(in.hx, in.hy));
    in.border = std::max(q.border, 0.0f) * k;
    // The falloff is centred on the edge: half of it lies outside the rect.
    in.pad = in.softness * 0.5f;

    // Wholly outside the clip (padding included): nothing to draw.
    const float px0 = mTarget.vpX + in.cx - in.hx - in.pad;
    const float py0 = mTarget.vpY + in.cy - in.hy - in.pad;
    const float px1 = mTarget.vpX + in.cx + in.hx + in.pad;
    const float py1 = mTarget.vpY + in.cy + in.hy + in.pad;
    if (px1 <= (float)clip.x || py1 <= (float)clip.y || px0 >= (float)(clip.x + clip.w) ||
        py0 >= (float)(clip.y + clip.h)) {
        if (culled) *culled = true;
        return false;
    }

    in.u0 = q.u0;
    in.v0 = q.v0;
    in.u1 = q.u1;
    in.v1 = q.v1;
    CopyColor(in.tl, q.tl);
    CopyColor(in.tr, q.tr);
    CopyColor(in.bl, q.bl);
    CopyColor(in.br, q.br);
    // An untextured quad is drawn with the white texture in colour mode.
    in.mode = q.tex ? (float)(int)q.texMode : 0.0f;
    in.ndcX = 2.0f / mTarget.vpW;
    in.ndcY = 2.0f / mTarget.vpH;
    in.reserved = 0.0f;

    // Order is z-order, so only the LAST batch can take the quad.
    if (mBatches.empty() || mBatches.back().tex != q.tex || mBatches.back().scissor != clip) {
        mBatches.push_back({q.tex, clip, (uint32_t)mInstances.size(), 0});
    }
    mBatches.back().count++;
    mInstances.push_back(in);
    return true;
}

void Canvas2DBuilder::CornerNdc(const Canvas2DInstance& in, int i, float out[2]) {
    // Must match vs_canvas in Canvas2D.cpp.
    static const float kCorner[6][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    const float px = in.cx + kCorner[i][0] * (in.hx + in.pad);
    const float py = in.cy + kCorner[i][1] * (in.hy + in.pad);
    out[0] = px * in.ndcX - 1.0f;
    out[1] = 1.0f - py * in.ndcY;
}
