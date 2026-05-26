// DC3 Web Port — WebGPU Device (Browser)
// Replaces GpuDevice.cpp for Emscripten builds.
// Uses emdawnwebgpu: same webgpu.h API but async init + canvas surface.

#ifdef __EMSCRIPTEN__

#include "gfx/GpuDevice.h"

#include <emscripten/emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/html5.h>

#include <cstdio>
#include <cstring>

// ============================================================================
// GpuDevice implementation (Emscripten/Browser)
// ============================================================================

GpuDevice::~GpuDevice() {
    Shutdown();
}

bool GpuDevice::Init(const GpuDeviceDesc& desc) {
    mHeadless = desc.headless;
    // The page resizer in index.html assigns to canvas.width/height to match
    // the viewport before WebGPU is initialized. The hardcoded 1280x720
    // defaults would then mismatch the live canvas and break surface configure.
    // Read the actual backing dimensions instead.
    int canvasW = EM_ASM_INT({
        var c = document.getElementById('dc3-canvas');
        return c ? c.width : 0;
    });
    int canvasH = EM_ASM_INT({
        var c = document.getElementById('dc3-canvas');
        return c ? c.height : 0;
    });
    mWidth = (canvasW > 0) ? canvasW : desc.width;
    mHeight = (canvasH > 0) ? canvasH : desc.height;

    // Create instance (no TimedWaitAny — browser doesn't support sync wait)
    mInstance = wgpu::CreateInstance(nullptr);
    if (!mInstance) {
        fprintf(stderr, "GpuDevice: failed to create WebGPU instance\n");
        return false;
    }

    // Start async adapter request — uses C++ template callback API
    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = wgpu::PowerPreference::HighPerformance;

    mInstance.RequestAdapter(
        &opts,
        wgpu::CallbackMode::AllowSpontaneous,
        [this](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, const char* message) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                fprintf(stderr, "GpuDevice: adapter request failed: %s\n", message ? message : "");
                return;
            }

            printf("GpuDevice: adapter acquired\n");
            mAdapter = std::move(adapter);
            mHasBCCompression = mAdapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);

            // Request device
            wgpu::DeviceDescriptor deviceDesc{};
            wgpu::FeatureName bcFeature = wgpu::FeatureName::TextureCompressionBC;
            if (mHasBCCompression) {
                deviceDesc.requiredFeatureCount = 1;
                deviceDesc.requiredFeatures = &bcFeature;
            }

            deviceDesc.SetDeviceLostCallback(
                wgpu::CallbackMode::AllowSpontaneous,
                [this](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
                    fprintf(stderr, "GpuDevice: device lost (reason=%d): %.*s\n",
                            (int)reason, (int)msg.length, msg.data);
                    mDeviceLost = true;
                });
            deviceDesc.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
                    fprintf(stderr, "GpuDevice: uncaptured error (type=%d): %.*s\n",
                            (int)type, (int)msg.length, msg.data);
                });

            mAdapter.RequestDevice(
                &deviceDesc,
                wgpu::CallbackMode::AllowSpontaneous,
                [this](wgpu::RequestDeviceStatus status, wgpu::Device device, const char* message) {
                    if (status != wgpu::RequestDeviceStatus::Success) {
                        fprintf(stderr, "GpuDevice: device request failed: %s\n", message ? message : "");
                        return;
                    }

                    mDevice = std::move(device);
                    mQueue = mDevice.GetQueue();

                    // Create canvas surface
                    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
                    canvasDesc.selector = "#dc3-canvas";

                    wgpu::SurfaceDescriptor surfDesc{};
                    surfDesc.nextInChain = &canvasDesc;
                    mSurface = mInstance.CreateSurface(&surfDesc);

                    if (!mSurface) {
                        fprintf(stderr, "GpuDevice: failed to create canvas surface\n");
                        return;
                    }

                    // Query the preferred surface format from the adapter
                    wgpu::SurfaceCapabilities caps;
                    mSurface.GetCapabilities(mAdapter, &caps);
                    if (caps.formatCount > 0 && caps.formats) {
                        mSurfaceFormat = caps.formats[0];
                        printf("GpuDevice: preferred surface format: %d\n", (int)mSurfaceFormat);
                    } else {
                        mSurfaceFormat = wgpu::TextureFormat::BGRA8Unorm;
                        printf("GpuDevice: no preferred format, using BGRA8Unorm\n");
                    }
                    ConfigureSurface();

                    printf("GpuDevice: initialized (%dx%d, web)\n", mWidth, mHeight);
                });
        });

    printf("GpuDevice: requesting adapter...\n");
    return true;
}

void GpuDevice::Shutdown() {
    mSamplerCache.clear();
    mHeadlessView = nullptr;
    mHeadlessTex = nullptr;
    mSurface = nullptr;
    mQueue = nullptr;
    mDevice = nullptr;
    mAdapter = nullptr;
    mInstance = nullptr;
}

void GpuDevice::ConfigureSurface() {
    wgpu::SurfaceConfiguration config{};
    config.device = mDevice;
    config.format = mSurfaceFormat;
    config.width = mWidth;
    config.height = mHeight;
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    mSurface.Configure(&config);
}

void GpuDevice::ResizeSurface(int width, int height) {
    if (width <= 0 || height <= 0) return;
    mWidth = width;
    mHeight = height;
    if (mSurface && mDevice) ConfigureSurface();
}

wgpu::TextureView GpuDevice::AcquireNextFrame() {
    if (!mSurface || !mDevice) return nullptr;

    wgpu::SurfaceTexture surfTex;
    mSurface.GetCurrentTexture(&surfTex);
    if (surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        return nullptr;
    }
    mSurfaceTex = surfTex.texture;
    return mSurfaceTex.CreateView();
}

void GpuDevice::PresentFrame() {
    // Browser auto-presents at end of rAF — explicit Present() is unsupported
    // by emdawnwebgpu. The surface texture returned by GetCurrentTexture() is
    // composited to the canvas when the requestAnimationFrame callback returns.
    if (mInstance) mInstance.ProcessEvents();
}

wgpu::TextureView GpuDevice::AcquireHeadlessFrame() {
    return AcquireNextFrame();
}

bool GpuDevice::ReadbackHeadlessFrame(uint8_t*, size_t) {
    return false;
}

bool GpuDevice::ShouldClose() const { return false; }

void GpuDevice::PollEvents() {
    if (mInstance) mInstance.ProcessEvents();
}

wgpu::Sampler GpuDevice::GetSampler(const SamplerDesc& desc) {
    auto it = mSamplerCache.find(desc);
    if (it != mSamplerCache.end()) return it->second;

    wgpu::SamplerDescriptor sd{};
    sd.addressModeU = desc.addressU;
    sd.addressModeV = desc.addressV;
    sd.addressModeW = wgpu::AddressMode::Repeat;
    sd.minFilter = desc.minFilter;
    sd.magFilter = desc.magFilter;
    sd.mipmapFilter = desc.mipmapFilter;
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = 16.0f;
    sd.maxAnisotropy = 1;

    wgpu::Sampler sampler = mDevice.CreateSampler(&sd);
    mSamplerCache[desc] = sampler;
    return sampler;
}

#endif // __EMSCRIPTEN__
