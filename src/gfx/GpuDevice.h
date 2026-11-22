#pragma once

#include <webgpu/webgpu_cpp.h>
#include <deque>
#include <string>
#include <unordered_map>

#include "gfx/GpuTimers.h"
#include "platform/LookProfile.h"

// Unconditional: mReadbackScale (a scaled frame's readback) is a member on
// every platform, web included, even though only desktop presents through one.
#include "gfx/PresentScale.h"

#ifndef __EMSCRIPTEN__
#include "gfx/PipelineDiskCache.h"
struct GLFWwindow;
#endif

struct GpuDeviceDesc {
    bool headless = false;
    // Headless: the frame. Windowed: the window's initial size (screen
    // coordinates; HiDPI scaling is the window system's, see InitWindow).
    int width = 1280;
    int height = 720;
    const char* title = "DC3 Native";
    // Windowed only. Non-zero: every frame is drawn at frameWidth x
    // frameHeight into a texture GpuDevice owns, and PresentFrame scales it
    // into the window, aspect kept, black bars where the shapes differ
    // (gfx/PresentScale.h). WindowWidth/Height are then the FRAME's size --
    // what every render target, viewport and readback is sized from -- and
    // SurfaceWidth/Height the window's. Zero: the swapchain is the frame and
    // follows the window (the behaviour before dc3 PLAN H-18; rb3's own
    // GpuDevice boot keeps it).
    int frameWidth = 0;
    int frameHeight = 0;
    // Open fullscreen on the monitor (its desktop mode, no mode switch).
    // MILO_FULLSCREEN=1 does the same. F11 or Alt+Enter toggles at run time.
    bool fullscreen = false;
    // Internal render scale (supersampling). > 1: the frame -- headless, or the
    // windowed fixed-size frame -- is drawn at the scale times its size and
    // box-filtered back down where it leaves the renderer: PresentFrame into
    // the window, ReadbackFrame/ReadbackHeadlessFrame into the size a caller
    // reads (OutputWidth/Height). WindowWidth/Height are the DRAWN size, so
    // every render target and viewport follows. 1 (the default): off, nothing
    // changes. Ignored windowed without a fixed-size frame, and on the web.
    // The scale actually used is ResolveRenderScale (below):
    //   renderScale / renderScaleIntegrated  the DEFAULT, by adapter class
    //       (discrete / anything else: integrated, CPU, unknown -- the scale
    //       multiplies the main pass's fill by its square, which a handheld's
    //       iGPU cannot carry). Applies only to a frame presented to a real
    //       surface; capped so the drawn frame stays within 3840x2160.
    //   renderScaleExplicit  asked for (0 = not): clamped to [1, 2], used
    //       headless too, on any adapter, and not capped.
    float renderScale = 1.0f;
    float renderScaleIntegrated = 1.0f;
    float renderScaleExplicit = 0.0f;
    // The frame at the output's own resolution (dc3 np-outres). Set (with a
    // logical size): the frame's OUTPUT size is the largest logicalWidth x
    // logicalHeight-shaped rectangle that fits the target -- the window's
    // surface, or headless the width x height frame -- so a 1920x1080 window
    // or headless frame is a 1920x1080 frame, and a 21:9 window is pillarboxed
    // instead of stretched. The frame is DRAWN at that times the render scale,
    // and windowed it follows every resize (frameWidth is then not used). The
    // logical size is the consumer's virtual resolution (the Rnd's 1280x720),
    // which viewports, UI layout and the post chain's pixel measures are
    // defined in: LogicalWidth/Height, and the frame's scale from it is
    // drawn / logical (FrameScaleX/Y), which need not be whole. Unset: the
    // frame is the target (headless) or frameWidth x frameHeight (windowed),
    // as before. The logical size is then still the consumer's when it gives
    // one (a frame that does not follow, drawn at the target's size, keeps its
    // viewports in the virtual resolution: dc3's retail headless at
    // MILO_WIDTH x MILO_HEIGHT), and the output size when it gives none.
    bool outputFollowsTarget = false;
    int logicalWidth = 0;
    int logicalHeight = 0;
    // A following frame's OUTPUT height cap, in rows (0 = no cap), applied
    // after the fit (ResolveFrameSizes); the output keeps the logical shape,
    // and the present path scales it up into the window as it scaled the
    // fixed frame before:
    //   outputMaxRowsIntegrated  the DEFAULT, on a non-discrete adapter
    //       presenting to a window (a handheld's iGPU, measured at 2.7-3.0 ms
    //       per 720p frame, would otherwise draw its whole 2560x1600 panel);
    //       discrete adapters and headless frames are not capped by default
    //   outputMaxRowsExplicit  asked for (< 0 = not): 0 = no cap, > 0 = that
    //       cap, on any adapter, headless included
    int outputMaxRowsIntegrated = 0;
    int outputMaxRowsExplicit = -1;
    // The web build's cap on an adapter without texture-compression-bc
    // (WebOutputRowCap; 0 = none).
    int outputMaxRowsNoBc = 0;
};

// The render scale rule (pure; GpuDevice::Init applies it).
//   renderScaleExplicit > 0  that, clamped to [1, kRenderScaleMax]
//   headless (no surface)  1: many headless engines run at once on a test box
//                          (gates, flow tests with a wall-clock limit), and a
//                          4x main-pass fill on each is a fleet-wide slowdown
//   otherwise              the default for the adapter class (renderScale
//                          on a discrete GPU, renderScaleIntegrated on any
//                          other), capped so the drawn frame fits
//                          kRenderScaleCapWidth x kRenderScaleCapHeight, and
//                          rounded DOWN to a whole number (a fractional box
//                          filter softens everything: 1.5x measured blurrier
//                          than 1x on world textures, 0.53 vs 0.77 of Xenia's
//                          Laplacian energy, at 2.4x the GPU time); floor 1.
// outputW/H: the size the frame leaves the renderer at (0 = unknown, no cap).
constexpr float kRenderScaleMax = 2.0f;
constexpr int kRenderScaleCapWidth = 3840;
constexpr int kRenderScaleCapHeight = 2160;
float ResolveRenderScale(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface,
                         int outputW, int outputH);

// The frame's three sizes for a target (the window's surface, or the headless
// frame) of targetW x targetH -- the one rule for every window size (pure;
// GpuDevice applies it at Init and at every resize):
//   output  outputFollowsTarget with a logical size: the logical shape fitted
//           into the target (FitPresentRect), then held to the row cap
//           (OutputRowCap: outputMaxRowsExplicit when asked, else
//           outputMaxRowsIntegrated on a non-discrete adapter presenting
//           to a window, else none) at the same shape; else windowed
//           frameWidth x frameHeight when set; else the target
//   scale   ResolveRenderScale(output)
//   drawn   output * scale
// So a 1280x720 window draws 2560x1440 and boxes it down 2x (the enhanced
// look's desktop default), 1920x1080 draws 3840x2160 and boxes 2x, 2560x1440
// and 3840x2160 draw 1x at their own size; headless draws 1x at its size.
// On an integrated adapter (enhanced: cap 1080) a 2560x1600 panel's frame is
// 1920x1080, scaled up into the window.
int OutputRowCap(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface);
struct FrameSizes {
    int outputW = 0, outputH = 0;
    float scale = 1.0f;
    int drawnW = 0, drawnH = 0;
};
FrameSizes ResolveFrameSizes(const GpuDeviceDesc& desc, bool discreteGpu, bool presentsToSurface,
                             int targetW, int targetH);
// The same with the row cap given (rowCap rows, 0 = none) instead of
// OutputRowCap's: the web build, whose cap is WebOutputRowCap's.
FrameSizes ResolveFrameSizesWithCap(const GpuDeviceDesc& desc, bool discreteGpu,
                                    bool presentsToSurface, int targetW, int targetH, int rowCap);
// A row cap set while running (the thermal governor's, gfx/ThermalGovernor.h)
// on top of the rules' own (baseCap, as OutputRowCap gives it). Pure.
//   TighterRowCap  the smaller of two caps, 0 on either side meaning none
//   ResolveFrameSizesThermal  runtimeCap 0, or one that does not make the
//       output shorter than baseCap alone does: exactly
//       ResolveFrameSizesWithCap(baseCap). Otherwise the output under the
//       tighter cap, at a render scale no larger than the uncapped frame's --
//       on a discrete adapter a 2560x1440 window draws 1x (2x would pass the
//       3840x2160 limit), and capped to 1920x1080 the default rule would
//       supersample it 2x: 3840x2160 drawn, more pixels than before the "step
//       down". A thermal cap never draws more than the frame it replaces.
int TighterRowCap(int a, int b);
FrameSizes ResolveFrameSizesThermal(const GpuDeviceDesc& desc, bool discreteGpu,
                                    bool presentsToSurface, int targetW, int targetH,
                                    int baseCap, int runtimeCap);
// The smallest following output presented to a window or canvas, in rows
// (the logical shape kept): a collapsed panel or a tiny embedded page must not
// make a 40x22 frame (the post chain's quarter- and bloom-size targets would
// be a few texels, or none). The frame is then larger than its box and is
// scaled down into it. Headless frames are the size asked for.
constexpr int kOutputMinRows = 360;

// The web build's row cap and why (printed at boot: it is a heuristic). A
// browser cannot say whether its adapter is discrete, so:
//   knob     outputMaxRowsExplicit >= 0 (MILO_OUTPUT_MAX_ROWS through ?env=):
//            that, both directions, on any adapter
//   no BC    an adapter without texture-compression-bc: outputMaxRowsNoBc
//            (720). Every phone and tablet GPU lacks it, every desktop
//            browser's adapter has it, and it is what ?gpu_profile=mobile|bare
//            mask; until a phone is measured its frame stays at 720 rows
//   default  otherwise outputMaxRowsIntegrated (1080), the cap an integrated
//            desktop adapter gets
enum class RowCapSource { None, Knob, NoBcAdapter, WebDefault };
struct RowCapChoice {
    int rows = 0;  // 0 = no cap
    RowCapSource source = RowCapSource::None;
};
RowCapChoice WebOutputRowCap(const GpuDeviceDesc& desc, bool adapterHasBc);
// The desc the web build resolves its frame's sizes from: the consumer's,
// at render scale 1 whatever it asked for (the web never supersamples; the
// browser's scaling of the canvas is the present).
GpuDeviceDesc WebFrameDesc(const GpuDeviceDesc& desc);
// "720 rows: adapter lacks texture-compression-bc", "1080 rows: web default",
// "0 rows (no cap): knob", ... into buf.
const char* DescribeRowCap(const RowCapChoice& c, char* buf, int bufSize);

// A size that must hold for settleMs before it is taken: a browser's resize
// and fullscreen events come in bursts, and each taken size re-creates every
// size-keyed target. Offer() the size seen at nowMs (milliseconds, any
// monotonic origin) each frame; it returns true, once, when a size other than
// the last taken one has been seen unchanged for settleMs, and takes it.
class SettledSize {
public:
    explicit SettledSize(double settleMs) : mSettleMs(settleMs) {}
    void Take(int w, int h) { mTakenW = mSeenW = w; mTakenH = mSeenH = h; }
    bool Offer(int w, int h, double nowMs);
    int TakenWidth() const { return mTakenW; }
    int TakenHeight() const { return mTakenH; }
private:
    double mSettleMs;
    int mSeenW = 0, mSeenH = 0;
    double mSeenSince = 0.0;
    int mTakenW = 0, mTakenH = 0;
};
// The web build's settle interval for the canvas's size.
constexpr double kWebResizeSettleMs = 250.0;

// Why GpuDevice::Init failed.
enum class GpuInitFailure {
    NoFailure,  // (not "None": X11 defines None as a macro)
    NoWindow,         // no window system, or GLFW could not open a window
    SurfaceUnusable,  // a window opened but WebGPU cannot present to it on any
                      // window system tried (dc3 PLAN H-33)
    NoAdapter,
    NoDevice,
};

// Sampler descriptor for cache key
struct SamplerDesc {
    wgpu::AddressMode addressU = wgpu::AddressMode::Repeat;
    wgpu::AddressMode addressV = wgpu::AddressMode::Repeat;
    wgpu::FilterMode minFilter = wgpu::FilterMode::Linear;
    wgpu::FilterMode magFilter = wgpu::FilterMode::Linear;
    wgpu::MipmapFilterMode mipmapFilter = wgpu::MipmapFilterMode::Linear;
    // > 1 only with all three filters linear (WebGPU's rule); GetSampler
    // falls back to 1 otherwise.
    uint16_t maxAnisotropy = 1;
    // Highest mip level sampled (WebGPU lodMaxClamp). 16 = no limit; 1 limits
    // the sample to the base and the first mip, whatever the texture holds.
    float lodMaxClamp = 16.0f;

    bool operator==(const SamplerDesc& o) const {
        return addressU == o.addressU && addressV == o.addressV &&
               minFilter == o.minFilter && magFilter == o.magFilter &&
               mipmapFilter == o.mipmapFilter && maxAnisotropy == o.maxAnisotropy &&
               lodMaxClamp == o.lodMaxClamp;
    }

    // The WebGPU descriptor (shared by the desktop and web GpuDevice).
    // maxAnisotropy > 1 is legal only with all three filters linear; any other
    // combination falls back to 1.
    wgpu::SamplerDescriptor ToWgpu() const {
        wgpu::SamplerDescriptor sd{};
        sd.addressModeU = addressU;
        sd.addressModeV = addressV;
        sd.addressModeW = wgpu::AddressMode::Repeat;
        sd.minFilter = minFilter;
        sd.magFilter = magFilter;
        sd.mipmapFilter = mipmapFilter;
        sd.lodMinClamp = 0.0f;
        sd.lodMaxClamp = lodMaxClamp;
        const bool allLinear = minFilter == wgpu::FilterMode::Linear &&
                               magFilter == wgpu::FilterMode::Linear &&
                               mipmapFilter == wgpu::MipmapFilterMode::Linear;
        sd.maxAnisotropy = (allLinear && maxAnisotropy > 1) ? maxAnisotropy : 1;
        return sd;
    }
};

struct SamplerDescHash {
    size_t operator()(const SamplerDesc& s) const {
        size_t h = 0;
        h ^= std::hash<int>{}(static_cast<int>(s.addressU)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(s.addressV)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(s.minFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(s.magFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(s.mipmapFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(s.maxAnisotropy)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(s.lodMaxClamp) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Whether the device is created with Dawn's use_user_defined_labels_in_backend
// toggle (Vulkan object names): MILO_GPU_LABELS, else on under a Vulkan
// capture/debug layer. See GpuDevice.cpp.
bool GpuLabelsWanted();

class GpuDevice {
public:
    GpuDevice() = default;
    ~GpuDevice();

    bool Init(const GpuDeviceDesc& desc);
    void Shutdown();

    // Accessors
    bool IsReady() const { return mDevice != nullptr && !mDeviceLost; }
    bool IsDeviceLost() const { return mDeviceLost; }
    bool IsNullBackend() const { return mNullBackend; }
    wgpu::Device& Device() { return mDevice; }
    wgpu::Queue& Queue() { return mQueue; }
    wgpu::Instance& Instance() { return mInstance; }
#ifndef __EMSCRIPTEN__
    GLFWwindow* Window() { return mWindow; }
#endif

    GpuInitFailure InitFailure() const { return mInitFailure; }

    // Surface management
    wgpu::TextureFormat SurfaceFormat() const { return mSurfaceFormat; }
    void ResizeSurface(int width, int height);

    // Frame lifecycle
    wgpu::TextureView AcquireNextFrame();
    void PresentFrame();
    // The texture this frame is drawn into: the swapchain texture, or -- with
    // a fixed frame size (GpuDeviceDesc::frameWidth) -- the owned frame
    // texture PresentFrame scales into the swapchain.
    wgpu::Texture& SurfaceTexture() { return mFrameTex ? mFrameTex : mSurfaceTex; }
    // The window's surface size in pixels (equal to WindowWidth/Height
    // without a fixed frame size).
    int SurfaceWidth() const { return mSurfaceWidth; }
    int SurfaceHeight() const { return mSurfaceHeight; }
    bool HasFixedFrame() const { return (bool)mFrameTex; }
#ifndef __EMSCRIPTEN__
    // Where the last presented frame went in the window (surface pixels).
    PresentRect LastPresentRect() const { return mPresentRect; }
    // Map a point in window coordinates (GLFW cursor positions) to frame
    // pixels. False when the point is outside the picture (on a bar).
    bool WindowToFrame(double winX, double winY, double& frameX, double& frameY) const;

    // Fullscreen (dc3 PLAN H-18): borderless on the monitor the window is
    // mostly on, at that monitor's current mode; leaving restores the window's
    // last position and size. F11 and Alt+Enter toggle (applied in
    // PollEvents).
    void SetFullscreen(bool on);
    bool IsFullscreen() const;

    // A windowed surface that has stopped handing out frames for good (dc3
    // PLAN H-33): no frame acquired since Init for kSurfaceGiveUpSeconds. The
    // caller should stop with SurfaceFailureReason(); AcquireNextFrame never
    // returns immediately on a failure, so the loop does not spin meanwhile.
    bool SurfaceFailed() const { return mSurfaceFailed; }
    const std::string& SurfaceFailureReason() const { return mSurfaceFailureReason; }
    static constexpr double kSurfaceGiveUpSeconds = 5.0;
    // Which GLFW window system the window is on ("x11", "wayland", "cocoa").
    const char* WindowSystemName() const;
#endif

    // Every texture the renderer makes (MILO_GPU_TIMERS lists its render
    // targets: GpuTimers::NoteTarget).
    wgpu::Texture CreateTexture(const wgpu::TextureDescriptor& desc) {
        if (GpuTimers::Requested()) GpuTimers::Get().NoteTarget(desc);
        return mDevice.CreateTexture(&desc);
    }

    // Feature queries
    bool HasBCCompression() const { return mHasBCCompression; }

    // Sampler cache
    wgpu::Sampler GetSampler(const SamplerDesc& desc);

    // Headless rendering (offscreen)
    wgpu::TextureView AcquireHeadlessFrame();
    bool ReadbackHeadlessFrame(uint8_t* outPixels, size_t outSize);
    // Copy a w x h 4-byte-per-texel colour texture (needs CopySrc usage) into
    // outPixels (tightly packed rows). Synchronous; a debugging/capture aid.
    bool ReadbackTexture(const wgpu::Texture& tex, int w, int h, uint8_t* outPixels, size_t outSize);
    wgpu::Texture& HeadlessTex() { return mHeadlessTex; }
    // The frame being drawn, as RGBA8 rows: the headless target, or -- windowed
    // -- the swapchain texture, which exists only between AcquireNextFrame and
    // PresentFrame, so a windowed caller reads it from WgpuRnd's frame-ready
    // callback (after the frame's submit, before present). BGRA surfaces are
    // swizzled. False when there is no frame to read or the surface was not
    // configured with CopySrc.
    bool ReadbackFrame(uint8_t* outPixels, size_t outSize);

    // Synchronously spin up + drain the driver's first-use submission/allocator
    // machinery, so it never races a thread started right after (e.g. audio).
    void WarmUp();

    // Headless only, after the frame's last submit: process Dawn's events
    // (the windowed path does in PresentFrame) and keep at most
    // HeadlessFramesInFlight() frames queued on the GPU.  Without it nothing
    // bounds how far an uncapped headless loop runs ahead of a shared GPU,
    // and the driver's descriptor / command / staging pools grow to the
    // deepest backlog seen (PLAN H-45, H-75).  MILO_HEADLESS_FRAMES_IN_FLIGHT
    // (default 2; 0 = unbounded, the old behaviour).
    void EndHeadlessFrame();
    int HeadlessFramesInFlight() const { return mHeadlessMaxInFlight; }
    void SetHeadlessFramesInFlight(int n) { mHeadlessMaxInFlight = n < 0 ? 0 : n; }

    // Window queries
    bool ShouldClose() const;
    void PollEvents();
    // Whether the window has input focus. True with no window (headless, web).
    bool IsWindowFocused() const;
    // Called with true before, and false after, each call that can block on
    // the compositor (acquiring the window's frame, presenting it). A hidden
    // window waits there for about a second; the audio mute reads it
    // (audio/BackgroundMute.h). Desktop windows only.
    void SetCompositorWaitHook(void (*hook)(bool waiting)) { mCompositorWaitHook = hook; }
    int WindowWidth() const { return mWidth; }
    int WindowHeight() const { return mHeight; }
    // The size a frame leaves the renderer at (readback, presentation's
    // source): WindowWidth/Height unless GpuDeviceDesc::renderScale draws it
    // larger. Size a readback buffer from these.
    int OutputWidth() const { return mOutputWidth > 0 ? mOutputWidth : mWidth; }
    int OutputHeight() const { return mOutputHeight > 0 ? mOutputHeight : mHeight; }
    // Supersampled: drawn larger than the output, box-filtered down to it.
    bool RenderScaled() const { return mOutputWidth > 0 && (mOutputWidth != mWidth || mOutputHeight != mHeight); }
    // Drawn over output: the whole-number supersampling factor (1 if none).
    float SupersampleScale() const { return (float)mHeight / (float)OutputHeight(); }
    // The render scales a resize of this frame can give (ResolveRenderScale
    // over every output size): lo at the largest output, hi at the smallest.
    // Both the scale now for a frame that does not follow its target.
    void RenderScaleRange(float& lo, float& hi) const;
    // The consumer's virtual resolution (GpuDeviceDesc::logicalWidth), which
    // viewports, UI layout and post-chain pixel measures are defined in; the
    // output size when it gave none.
    int LogicalWidth() const { return mLogicalWidth > 0 ? mLogicalWidth : OutputWidth(); }
    int LogicalHeight() const { return mLogicalHeight > 0 ? mLogicalHeight : OutputHeight(); }
    // Drawn over logical: what a viewport or a pixel measure in the logical
    // resolution is multiplied by for the drawn frame (1.5 for a 1920x1080
    // frame at 1x, 3 at 2x; 2 for round 3's 1280x720 at 2x).
    float FrameScaleX() const { return (float)mWidth / (float)LogicalWidth(); }
    float FrameScaleY() const { return (float)mHeight / (float)LogicalHeight(); }
    bool FrameScaled() const { return mWidth != LogicalWidth() || mHeight != LogicalHeight(); }
    // The frame's scale (FrameScaleY: post measures follow the output's
    // height) for a target `w` pixels wide when `w` is the frame's drawn
    // width, else 1 (an offscreen target keeps its own pixels).
    float RenderScaleFor(int w) const {
        return FrameScaled() && w == mWidth ? FrameScaleY() : 1.0f;
    }
    // A render-scaled frame's box filter dithered to 8 bits, in the present
    // AND the readback alike (PresentScale::SetDither), so a screenshot is the
    // frame the window shows. The enhanced look's 16-bit post chain
    // (LookSettings::postPrecision) asks for it each frame; nothing changes at
    // scale 1 (no box filter) or with it off.
    // On web there is no present-side PresentScale (the canvas is the frame),
    // so only the readback's box filter carries it.
    void SetPresentDither(bool on) {
#ifndef __EMSCRIPTEN__
        mPresentScale.SetDither(on);
#endif
        mReadbackScale.SetDither(on);
    }
    // np-present60: a frame drawn smaller than its present rectangle read with
    // the clamped Catmull-Rom cubic instead of one bilinear tap
    // (PresentScale::SetUpscaleFilter; milo::LookPresentUpscale for
    // LookClass() asks each frame). The readback's PresentScale follows, so
    // the two never disagree;
    // a screenshot is the frame at its own size, which no upscale reaches.
    void SetPresentUpscale(bool on) {
        const PresentScale::UpscaleFilter f =
            on ? PresentScale::UpscaleFilter::CatmullRom5Clamp : PresentScale::UpscaleFilter::Bilinear;
#ifndef __EMSCRIPTEN__
        mPresentScale.SetUpscaleFilter(f);
#endif
        mReadbackScale.SetUpscaleFilter(f);
    }
    bool PresentUpscale() const {
        return mReadbackScale.GetUpscaleFilter() != PresentScale::UpscaleFilter::Bilinear;
    }
    bool PresentDither() const {
#ifndef __EMSCRIPTEN__
        if (!mPresentScale.Dither()) return false;
#endif
        return mReadbackScale.Dither();
    }
    // The adapter reported itself a discrete GPU (render scale's default).
    // Everything that sizes the frame reads this two-way flag; the look's
    // per-adapter rules read LookClass().
    bool IsDiscreteGpu() const { return mDiscreteGpu; }
    // np-look49: the adapter class the look's per-adapter rules are decided
    // by (milo::LookAdapterClass: the depth of field's near field and spread,
    // the texture pass's MSAA and size). The one accessor every caller of
    // milo::LookDofNearField / LookDofNearSpread / LookTexPassMsaa /
    // LookTexPassScale passes. OtherNonDiscrete before the adapter is known.
    // An inspection token can force it (milo::SetLookAdapterClassForInspection).
    milo::LookAdapterClass LookClass() const { return milo::LookAdapterClassInEffect(mLookAdapter); }
    // The facts LookClass() is decided from (recorded with mDiscreteGpu).
    const milo::LookAdapterFacts& LookAdapter() const { return mLookAdapter; }
    bool IsHeadless() const { return mHeadless; }
#ifndef __EMSCRIPTEN__
    // Dawn's persistent blob cache (gfx/PipelineDiskCache.h); IsOpen() false
    // when it is off. The web build has none: the browser caches.
    const PipelineDiskCache& PipelineCache() const { return mPipelineCache; }
#endif
#ifdef __EMSCRIPTEN__
    // The backing-store size of the consumer's canvas (MILO_WEB_CANVAS_SELECTOR,
    // which only TUs compiled into the consumer's web target see, so callers in
    // libmilo-engine.a ask here). False, with both sizes 0, when the page has no
    // such canvas.
    bool CanvasSize(int& width, int& height) const;
    // A following frame (GpuDeviceDesc::outputFollowsTarget) on web: once a
    // frame, the canvas's box in device pixels (the page's
    // window.miloCanvasDevicePixels, else its CSS box times devicePixelRatio),
    // taken once it has held kWebResizeSettleMs (SettledSize); the frame's
    // sizes are then re-resolved, the canvas's buffer set to the output and
    // the surface configured once. True when it reconfigured. No-op for a
    // frame that does not follow (it keeps the canvas's own buffer size).
    bool FollowCanvas();
    int FrameReconfigures() const { return mFrameReconfigures; }
    // The frame's shape on web: false (the default) fits the logical shape
    // (16:9) into the canvas's box; true fits it turned on its side (9:16),
    // for a phone held upright during a song (dc3 PLAN W-37 step 2). The
    // consumer's virtual resolution and its viewports do not change: a
    // full-screen viewport covers the whole portrait frame, so its cameras
    // must take their aspect from the frame (WindowWidth/Height). Taken at
    // the next FollowCanvas, with the canvas's settled box. The page sets it
    // with milo_web_set_portrait_frame(0|1); read back by
    // milo_web_portrait_frame() (the frame's shape, not the request).
    void SetPortraitFrame(bool on) { mPortraitWanted = on; }
    bool PortraitFrame() const { return mPortraitFrame; }
    void RegisterWebInstance();
#endif
    // The frame follows its target (GpuDeviceDesc::outputFollowsTarget).
    bool FollowsTarget() const { return mFollowsTarget; }
#ifndef __EMSCRIPTEN__
    // A row cap on top of the rules' own, changeable while running (the
    // thermal governor's, platform/ThermalState.h; 0 = none): the tighter of
    // the two is in force (ResolveFrameSizesThermal). The frame's sizes are
    // re-resolved and re-applied at once, as at a window resize, so call it
    // between frames (WgpuRnd::BeginDrawing does, before it acquires the
    // frame); everything sized from WindowWidth/OutputWidth follows on that
    // frame as it follows a resize. True when a size changed. Only a frame
    // that follows its target has a row cap; any other is left alone. The
    // web build has no such cap (a browser reports no thermal state).
    bool SetRuntimeRowCap(int rows);
    int RuntimeRowCap() const { return mRuntimeRowCap; }
    // The output's height under the rules alone, without the runtime cap
    // (what the governor steps down from); 0 for a frame that does not follow.
    int BaseOutputRows() const;
#endif

private:
#ifndef __EMSCRIPTEN__
    bool InitInstance();
    bool InitAdapter();
    bool InitDevice();
    bool InitWindow(const GpuDeviceDesc& desc, int platform);
    bool InitSurface();
    bool InitPresentation(const GpuDeviceDesc& desc);
    bool SurfaceUsable(std::string& why);
    void DestroyWindow();
    void QuerySurfaceSize(int& w, int& h) const;
    void CreateFrameTexture();
    void ApplyPendingFullscreenToggle();
    static void KeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods);
#endif
    void ConfigureSurface();

    wgpu::Instance mInstance;
    wgpu::Adapter mAdapter;
    wgpu::Device mDevice;
    wgpu::Queue mQueue;
    wgpu::Surface mSurface;
    wgpu::TextureFormat mSurfaceFormat = wgpu::TextureFormat::BGRA8Unorm;

    bool mNullBackend = false;
    bool mDiscreteGpu = false;
    milo::LookAdapterFacts mLookAdapter;
#ifndef __EMSCRIPTEN__
    PipelineDiskCache mPipelineCache;
    std::string mPipelineCacheIsolation;  // Dawn keeps a view of it
    uint32_t mAdapterVendorId = 0, mAdapterDeviceId = 0;
    std::string mAdapterDescription;
    GLFWwindow* mWindow = nullptr;
    void (*mPrevKeyCallback)(GLFWwindow*, int, int, int, int) = nullptr;
    bool mFullscreenToggleRequested = false;
    int mWindowedX = 0, mWindowedY = 0, mWindowedW = 0, mWindowedH = 0;
    PresentScale mPresentScale;
    int mLoggedPresentUpscale = -1;      // np-present60: the present's filter line
    bool mLoggedPresentStretched = false;
    PresentRect mPresentRect;
    bool mSurfaceNeedsConfigure = false;
    bool mEverAcquired = false;
    bool mSurfaceFailed = false;
    std::string mSurfaceFailureReason;
    long long mAcquireFailures = 0;
    double mFirstFailureTime = 0.0;
    int mSurfaceFault = 0;  // MILO_SURFACE_FAULT (test hook), see GpuDevice.cpp
#endif
    GpuInitFailure mInitFailure = GpuInitFailure::NoFailure;
    // The frame's size: WindowWidth/Height.
    int mWidth = 0;
    int mHeight = 0;
    // GpuDeviceDesc::renderScale: the frame's output size (0 = not scaled),
    // and the box-filtered copy a scaled frame is read back through.
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    // GpuDeviceDesc::logicalWidth (0 = the output size) and the desc the
    // frame's sizes are re-resolved from at a resize (outputFollowsTarget).
    int mLogicalWidth = 0;
    int mLogicalHeight = 0;
    bool mFollowsTarget = false;
    GpuDeviceDesc mSizeDesc;
    // The output's row cap in force (OutputRowCap; WebOutputRowCap on web),
    // the rules' own cap (mBaseRowCap), the cap set while running
    // (SetRuntimeRowCap; 0 = none) and whether that one is the cap in force.
    int mRowCap = 0;
    int mBaseRowCap = 0;
    int mRuntimeRowCap = 0;
    bool mRowCapIsRuntime = false;
#ifdef __EMSCRIPTEN__
    RowCapChoice mWebRowCap;
    SettledSize mCanvasSize{kWebResizeSettleMs};
    bool mPortraitWanted = false;
    bool mPortraitFrame = false;
    int mFrameReconfigures = 0;
    bool FollowTarget(int targetW, int targetH, bool atInit);
    bool CanvasTargetSize(int& w, int& h) const;
#endif
    void ApplyFrameSizes(const FrameSizes& s, bool atInit = false);
#ifndef __EMSCRIPTEN__
    FrameSizes ResolveSizesNow(int targetW, int targetH);
#endif
    PresentScale mReadbackScale;
    wgpu::Texture mReadbackTex;
    bool ReadbackScaled(const wgpu::Texture& src, wgpu::TextureFormat fmt, uint8_t* outPixels,
                        size_t outSize);
    // The swapchain's configured size.
    int mSurfaceWidth = 0;
    int mSurfaceHeight = 0;
    bool mHeadless = false;
    int mHeadlessMaxInFlight = 2;
    std::deque<wgpu::Future> mHeadlessInFlight;
    bool mHasBCCompression = false;
    bool mDeviceLost = false;

    // Surface texture (stored for CopyTextureToTexture in web frame resolve)
    wgpu::Texture mSurfaceTex;
    void (*mCompositorWaitHook)(bool waiting) = nullptr;
    // The fixed-size frame (GpuDeviceDesc::frameWidth), windowed only.
    wgpu::Texture mFrameTex;
    wgpu::TextureView mFrameView;
    bool mSurfaceCopySrc = false; // surface configured with CopySrc usage

    // Headless offscreen target
    wgpu::Texture mHeadlessTex;
    wgpu::TextureView mHeadlessView;

    std::unordered_map<SamplerDesc, wgpu::Sampler, SamplerDescHash> mSamplerCache;
};
