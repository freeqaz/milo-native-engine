#pragma once
//
// PresentScale -- the last step of a windowed frame (dc3 PLAN H-18).
//
// The game draws its frame at a fixed size (DC3: the Rnd virtual resolution,
// 1280x720), into a texture GpuDevice owns. PresentScale copies that frame into
// the window's swapchain texture, scaled to the largest rectangle of the frame's
// aspect that fits the window and centred in it; the rest of the window is black
// (letterbox above/below, pillarbox left/right). Nothing is stretched.
//
// Before this, the swapchain WAS the frame: it followed the window, while the
// game's viewports and UI layout follow the Rnd virtual resolution, so a window
// larger than 1280x720 -- resized, maximised, fullscreen, a tiling desktop, a
// HiDPI framebuffer -- showed the picture in its top-left corner.
//
// It runs after everything the frame draws (post chain, display gamma ramp,
// ImGui), so a full-frame pass over the finished frame still sees the frame at
// its own size and composes with this unchanged.
//
// rndobj-free (gfx core tier).

#include <webgpu/webgpu_cpp.h>

struct PresentRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PresentRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

// The largest frameW:frameH rectangle that fits a surfaceW x surfaceH surface,
// centred (integer pixels; the leftover splits evenly, odd pixel to the
// right/bottom). Zero-sized when either size is not positive.
PresentRect FitPresentRect(int frameW, int frameH, int surfaceW, int surfaceH);

// Whether the fitted rectangle is a whole-number scale of the frame (1x, 2x,
// ...): those are sampled nearest, so a 2560x1440 HiDPI framebuffer shows the
// 1280x720 frame as exact 2x2 pixels; any other scale is filtered (linear).
bool PresentRectIsIntegerScale(const PresentRect& r, int frameW, int frameH);

class PresentScale {
public:
    // Encodes the scaled copy of `frame` (a TextureBinding texture) into
    // `target` (the swapchain texture; RenderAttachment, `targetFormat`), the
    // rest of `target` cleared to black. Returns the rectangle drawn into.
    PresentRect Encode(wgpu::Device& device, wgpu::CommandEncoder& encoder,
                       const wgpu::Texture& frame, const wgpu::TextureView& target,
                       wgpu::TextureFormat targetFormat, int targetW, int targetH);

private:
    wgpu::ShaderModule mShader;
    wgpu::BindGroupLayout mBGL;
    wgpu::RenderPipeline mPipe;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    wgpu::Sampler mLinear, mNearest;
    // The bind group for the last frame texture and filter (the frame texture
    // is recreated only when its size or format changes).
    wgpu::Texture mBoundFrame;
    bool mBoundNearest = false;
    wgpu::BindGroup mBindGroup;
};

// The WGSL PresentScale compiles (gfx/ShippedWgsl.h).
const char* PresentScaleWgslSource();
