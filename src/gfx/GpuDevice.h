#pragma once

#include <webgpu/webgpu_cpp.h>
#include <string>
#include <unordered_map>

#ifndef __EMSCRIPTEN__
struct GLFWwindow;
#endif

struct GpuDeviceDesc {
    bool headless = false;
    int width = 1280;
    int height = 720;
    const char* title = "DC3 Native";
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

    // Surface management
    wgpu::TextureFormat SurfaceFormat() const { return mSurfaceFormat; }
    void ResizeSurface(int width, int height);

    // Frame lifecycle
    wgpu::TextureView AcquireNextFrame();
    void PresentFrame();
    wgpu::Texture& SurfaceTexture() { return mSurfaceTex; }

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
    bool InitWindow(const GpuDeviceDesc& desc);
    bool InitSurface();
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
#endif
    int mWidth = 0;
    int mHeight = 0;
    bool mHeadless = false;
    bool mHasBCCompression = false;
    bool mDeviceLost = false;

    // Surface texture (stored for CopyTextureToTexture in web frame resolve)
    wgpu::Texture mSurfaceTex;
    bool mSurfaceCopySrc = false; // surface configured with CopySrc usage

    // Headless offscreen target
    wgpu::Texture mHeadlessTex;
    wgpu::TextureView mHeadlessView;

    std::unordered_map<SamplerDesc, wgpu::Sampler, SamplerDescHash> mSamplerCache;
};
