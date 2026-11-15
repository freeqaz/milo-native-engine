// The frame's sizes: the pure rules (ResolveRenderScale, OutputRowCap,
// ResolveFrameSizes; gfx/GpuDevice.h) and GpuDevice::ApplyFrameSizes, which
// applies them. Their own TU so the web build (GpuDevice_Web.cpp, which
// replaces GpuDevice.cpp) runs the same rules as the desktop.
#include "gfx/GpuDevice.h"

#include <cstdio>

float ResolveRenderScale(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface,
                         int outputW, int outputH) {
    const float explicitScale = desc.renderScaleExplicit;
    const float defaultScale = discreteGpu ? desc.renderScale : desc.renderScaleIntegrated;
    if (explicitScale > 0.0f)
        return explicitScale < 1.0f ? 1.0f
             : explicitScale > kRenderScaleMax ? kRenderScaleMax : explicitScale;
    if (!presentsToSurface || !(defaultScale > 1.0f)) return 1.0f;
    float s = defaultScale > kRenderScaleMax ? kRenderScaleMax : defaultScale;
    if (outputW > 0 && s * (float)outputW > (float)kRenderScaleCapWidth)
        s = (float)kRenderScaleCapWidth / (float)outputW;
    if (outputH > 0 && s * (float)outputH > (float)kRenderScaleCapHeight)
        s = (float)kRenderScaleCapHeight / (float)outputH;
    s = (float)(int)s;  // whole numbers only (see GpuDevice.h)
    return s < 1.0f ? 1.0f : s;
}

int OutputRowCap(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface) {
    if (desc.outputMaxRowsExplicit >= 0) return desc.outputMaxRowsExplicit;
    if (discreteGpu || !presentsToSurface) return 0;
    return desc.outputMaxRowsIntegrated > 0 ? desc.outputMaxRowsIntegrated : 0;
}

FrameSizes ResolveFrameSizes(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface,
                             int targetW, int targetH) {
    FrameSizes s;
    if (desc.outputFollowsTarget && desc.logicalWidth > 0 && desc.logicalHeight > 0) {
        const PresentRect r = FitPresentRect(desc.logicalWidth, desc.logicalHeight, targetW, targetH);
        s.outputW = r.w;
        s.outputH = r.h;
        const int cap = OutputRowCap(desc, discreteGpu, presentsToSurface);
        if (cap > 0 && s.outputH > cap) {
            // The same shape, cap rows tall (1920x1080 for 16:9 at 1080).
            s.outputW = (int)((double)r.w * cap / r.h + 0.5);
            s.outputH = cap;
        }
    } else if (presentsToSurface && desc.frameWidth > 0 && desc.frameHeight > 0) {
        s.outputW = desc.frameWidth;
        s.outputH = desc.frameHeight;
    } else {
        s.outputW = targetW;
        s.outputH = targetH;
    }
    s.scale = ResolveRenderScale(desc, discreteGpu, presentsToSurface, s.outputW, s.outputH);
    s.drawnW = (int)(s.outputW * s.scale + 0.5f);
    s.drawnH = (int)(s.outputH * s.scale + 0.5f);
    return s;
}

// The frame's sizes (ResolveFrameSizes): drawn = WindowWidth/Height, output
// kept only when it differs (OutputWidth falls back to the drawn size), and
// the box filter on exactly when the frame is drawn larger than its output.
void GpuDevice::ApplyFrameSizes(const FrameSizes& s, bool atInit) {
    const int prevW = mWidth, prevH = mHeight, prevOutW = OutputWidth(), prevOutH = OutputHeight();
    mWidth = s.drawnW;
    mHeight = s.drawnH;
    const bool scaled = s.drawnW != s.outputW || s.drawnH != s.outputH;
    mOutputWidth = scaled ? s.outputW : 0;
    mOutputHeight = scaled ? s.outputH : 0;
#ifndef __EMSCRIPTEN__
    mPresentScale.SetBoxFilter(scaled);
#endif
    mReadbackScale.SetBoxFilter(scaled);
    if (!atInit && s.drawnW == prevW && s.drawnH == prevH && OutputWidth() == prevOutW &&
        OutputHeight() == prevOutH)
        return;
    const int rowCap = mFollowsTarget ? OutputRowCap(mSizeDesc, mDiscreteGpu, !mHeadless) : 0;
    char capText[48] = "";
    if (rowCap > 0)
        snprintf(capText, sizeof(capText), ", output capped at %d rows%s", rowCap,
                 mSizeDesc.outputMaxRowsExplicit >= 0 ? " (asked for)" : "");
    printf("GpuDevice: frame %dx%d out, render scale %.2f%s on a %s GPU (%dx%d drawn, logical "
           "%dx%d%s)\n",
           OutputWidth(), OutputHeight(), s.scale,
           mSizeDesc.renderScaleExplicit > 0.0f ? " (asked for)" : "",
           mDiscreteGpu ? "discrete" : "non-discrete", mWidth, mHeight, LogicalWidth(),
           LogicalHeight(), capText);
}
