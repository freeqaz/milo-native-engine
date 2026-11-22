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

    // Render scale (GpuDeviceDesc::renderScale): a frame drawn larger than the
    // rectangle it lands in is box-filtered -- four bilinear taps spread over
    // each output pixel's footprint, an exact 2x2 box at 2x -- instead of one
    // bilinear tap, which skips source pixels below 0.5x. Off by default: the
    // window-fit present samples as it always has.
    void SetBoxFilter(bool on) { mBoxFilter = on; }
    // The box filter's output dithered to 8 bits (fs_box_dither): a fixed 4x4
    // ordered pattern that leaves every exact 8-bit level exact -- flat fills
    // and glyph interiors come out as drawn -- and keeps the fraction of a
    // level the 2x2 mean carries (the enhanced look's 16-bit post chain,
    // LookSettings::postPrecision, under a render scale). Only with the box
    // filter, into an RGBA8Unorm/BGRA8Unorm target; off by default.
    void SetDither(bool on) { mDither = on; }
    bool Dither() const { return mDither; }
    // The frame's alpha copied through, whichever filter runs: the one-tap
    // case takes fs_copy instead of the opaque fs_present (the box filter has
    // always averaged alpha). For a copy that is not the present. Off by
    // default: a window is not composited through the frame's alpha.
    void SetKeepAlpha(bool on) { mKeepAlpha = on; }
    // The render pass's label (GPU timers), when the copy is not the present.
    void SetPassLabel(const char* label) { mLabel = label; }

    // Upscale: a frame drawn SMALLER than its rectangle (a row cap, a thermal
    // step) read with a Catmull-Rom cubic instead of one bilinear tap. The
    // numbers are texture reads per output pixel: 9 or 5 bilinear reads, plus
    // 3 gathers for the clamp to the four nearest texels (no overshoot, so no
    // halo beside text). Bilinear (the default) is today's one tap; a scale of
    // 1, a downscale (the box filter), the alpha-keeping copy and, unless
    // SetUpscaleAtWholeScales, a whole-number scale (nearest) never use it.
    enum class UpscaleFilter { Bilinear = 0, CatmullRom9, CatmullRom9Clamp, CatmullRom5, CatmullRom5Clamp };
    void SetUpscaleFilter(UpscaleFilter f) { mUpscale = f; }
    UpscaleFilter GetUpscaleFilter() const { return mUpscale; }
    void SetUpscaleAtWholeScales(bool on) { mUpscaleAtWholeScales = on; }
    // The filter the last Encode drew with (Bilinear when none applied).
    UpscaleFilter LastUpscale() const { return mLastUpscale; }

private:
    UpscaleFilter mUpscale = UpscaleFilter::Bilinear;
    UpscaleFilter mLastUpscale = UpscaleFilter::Bilinear;
    bool mUpscaleAtWholeScales = false;
    wgpu::RenderPipeline mUpPipe[5];
    bool mBoxFilter = false;
    bool mDither = false;
    bool mKeepAlpha = false;
    wgpu::RenderPipeline mBoxDitherPipe;
    wgpu::RenderPipeline mCopyPipe;
    const char* mLabel = "PresentScalePass";
    wgpu::RenderPipeline mBoxPipe;
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
