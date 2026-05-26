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
    wgpu::Texture& HeadlessTex() { return mHeadlessTex; }

    // Window queries
    bool ShouldClose() const;
    void PollEvents();
    int WindowWidth() const { return mWidth; }
    int WindowHeight() const { return mHeight; }
    bool IsHeadless() const { return mHeadless; }

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

    // Headless offscreen target
    wgpu::Texture mHeadlessTex;
    wgpu::TextureView mHeadlessView;

    std::unordered_map<SamplerDesc, wgpu::Sampler, SamplerDescHash> mSamplerCache;
};
