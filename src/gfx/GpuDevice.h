#pragma once

#include <webgpu/webgpu_cpp.h>
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
};

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

    bool operator==(const SamplerDesc& o) const {
        return addressU == o.addressU && addressV == o.addressV &&
               minFilter == o.minFilter && magFilter == o.magFilter &&
               mipmapFilter == o.mipmapFilter;
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

    // Window queries
    bool ShouldClose() const;
    void PollEvents();
    int WindowWidth() const { return mWidth; }
    int WindowHeight() const { return mHeight; }
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
    // The swapchain's configured size.
    int mSurfaceWidth = 0;
    int mSurfaceHeight = 0;
    bool mHeadless = false;
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
