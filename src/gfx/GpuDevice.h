#pragma once

#include <webgpu/webgpu_cpp.h>
#include <deque>
#include <string>
#include <unordered_map>

#ifndef __EMSCRIPTEN__
#include "gfx/PresentScale.h"
#endif

#ifndef __EMSCRIPTEN__
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
    int WindowWidth() const { return mWidth; }
    int WindowHeight() const { return mHeight; }
    // The size a frame leaves the renderer at (readback, presentation's
    // source): WindowWidth/Height unless GpuDeviceDesc::renderScale draws it
    // larger. Size a readback buffer from these.
    int OutputWidth() const { return mOutputWidth > 0 ? mOutputWidth : mWidth; }
    int OutputHeight() const { return mOutputHeight > 0 ? mOutputHeight : mHeight; }
    bool RenderScaled() const { return mOutputWidth > 0 && (mOutputWidth != mWidth || mOutputHeight != mHeight); }
    // Drawn width over output width for a target `w` pixels wide: the render
    // scale when `w` is the frame's drawn width, else 1 (an offscreen target).
    float RenderScaleFor(int w) const {
        return RenderScaled() && w == mWidth ? (float)mWidth / (float)OutputWidth() : 1.0f;
    }
    // A render-scaled frame's box filter dithered to 8 bits, in the present
    // AND the readback alike (PresentScale::SetDither), so a screenshot is the
    // frame the window shows. The enhanced look's 16-bit post chain
    // (LookSettings::postPrecision) asks for it each frame; nothing changes at
    // scale 1 (no box filter) or with it off.
    void SetPresentDither(bool on) {
        mPresentScale.SetDither(on);
        mReadbackScale.SetDither(on);
    }
    bool PresentDither() const { return mPresentScale.Dither() && mReadbackScale.Dither(); }
    // The adapter reported itself a discrete GPU (render scale's default).
    bool IsDiscreteGpu() const { return mDiscreteGpu; }
    bool IsHeadless() const { return mHeadless; }
#ifdef __EMSCRIPTEN__
    // The backing-store size of the consumer's canvas (MILO_WEB_CANVAS_SELECTOR,
    // which only TUs compiled into the consumer's web target see, so callers in
    // libmilo-engine.a ask here). False, with both sizes 0, when the page has no
    // such canvas.
    bool CanvasSize(int& width, int& height) const;
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
#ifndef __EMSCRIPTEN__
    GLFWwindow* mWindow = nullptr;
    void (*mPrevKeyCallback)(GLFWwindow*, int, int, int, int) = nullptr;
    bool mFullscreenToggleRequested = false;
    int mWindowedX = 0, mWindowedY = 0, mWindowedW = 0, mWindowedH = 0;
    PresentScale mPresentScale;
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
    // The fixed-size frame (GpuDeviceDesc::frameWidth), windowed only.
    wgpu::Texture mFrameTex;
    wgpu::TextureView mFrameView;
    bool mSurfaceCopySrc = false; // surface configured with CopySrc usage

    // Headless offscreen target
    wgpu::Texture mHeadlessTex;
    wgpu::TextureView mHeadlessView;

    std::unordered_map<SamplerDesc, wgpu::Sampler, SamplerDescHash> mSamplerCache;
};
