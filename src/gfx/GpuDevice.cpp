#include "gfx/GpuDevice.h"

#include <GLFW/glfw3.h>
#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>
#undef Success // X11 defines this, conflicts with wgpu enum
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
// Defined in MetalSurface.mm — creates CAMetalLayer for an NSWindow
extern "C" void* CreateMetalLayerForWindow(void* nsWindow);
#endif

#include <cstdio>
#include <cstring>

GpuDevice::~GpuDevice() {
    Shutdown();
}

bool GpuDevice::Init(const GpuDeviceDesc& desc) {
    mHeadless = desc.headless;
    mWidth = desc.width;
    mHeight = desc.height;

    if (!InitInstance()) return false;
    if (!mHeadless) {
        if (!InitWindow(desc)) return false;
        if (!InitSurface()) return false;  // Surface needed before adapter for compatibility
    }
    if (!InitAdapter()) return false;
    if (!InitDevice()) return false;

    if (!mHeadless) {
        ConfigureSurface();
    }

    printf("GpuDevice: initialized (%dx%d, %s)\n",
           mWidth, mHeight, mHeadless ? "headless" : "windowed");
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

    if (mWindow) {
        glfwDestroyWindow(mWindow);
        mWindow = nullptr;
        glfwTerminate();
    }
}

bool GpuDevice::InitInstance() {
    // TimedWaitAny required for synchronous adapter/device request
    static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDesc{};
    instanceDesc.requiredFeatureCount = 1;
    instanceDesc.requiredFeatures = &kTimedWaitAny;
    mInstance = wgpu::CreateInstance(&instanceDesc);
    if (!mInstance) {
        fprintf(stderr, "GpuDevice: failed to create WebGPU instance\n");
        return false;
    }
    return true;
}

bool GpuDevice::InitAdapter() {
    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = wgpu::PowerPreference::HighPerformance;

    // If we have a surface, pass it so the adapter can present to it
    if (mSurface) {
        opts.compatibleSurface = mSurface;
    }

    // Synchronous adapter request
    wgpu::Future future = mInstance.RequestAdapter(
        &opts,
        wgpu::CallbackMode::WaitAnyOnly,
        [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView msg) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                fprintf(stderr, "GpuDevice: adapter request failed: %.*s\n",
                        (int)msg.length, msg.data);
                return;
            }
            adapter = std::move(result);
        });
    mInstance.WaitAny(future, UINT64_MAX);

    if (!adapter) {
        fprintf(stderr, "GpuDevice: no WebGPU adapter found\n");
        return false;
    }

    // Print adapter info
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    printf("GpuDevice: GPU = %.*s (%.*s)\n",
           (int)info.device.length, info.device.data,
           (int)info.description.length, info.description.data);

    // Detect null/fallback backend (renders produce empty frames)
    mNullBackend = (info.backendType == wgpu::BackendType::Null);
    if (mNullBackend) {
        fprintf(stderr, "GpuDevice: WARNING — using Null backend (no real GPU). "
                "Renders will be black/empty.\n");
    }

    // Check for BC texture compression support
    mHasBCCompression = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    printf("GpuDevice: BC texture compression %s\n",
           mHasBCCompression ? "supported" : "NOT supported (will use CPU fallback)");

    mAdapter = std::move(adapter);
    return true;
}

bool GpuDevice::InitDevice() {
    wgpu::Device device;
    wgpu::DeviceDescriptor deviceDesc{};

    // Request BC compression feature if available
    wgpu::FeatureName requiredFeatures[1];
    if (mHasBCCompression) {
        requiredFeatures[0] = wgpu::FeatureName::TextureCompressionBC;
        deviceDesc.requiredFeatureCount = 1;
        deviceDesc.requiredFeatures = requiredFeatures;
    }

    // Enable Dawn toggle to propagate WebGPU labels to VK_EXT_debug_utils
    // (shows object names in RenderDoc, GFXReconstruct, validation layers)
    wgpu::DawnTogglesDescriptor toggles{};
    static const char* kEnabledToggles[] = {"use_user_defined_labels_in_backend"};
    toggles.enabledToggleCount = 1;
    toggles.enabledToggles = kEnabledToggles;
    deviceDesc.nextInChain = &toggles;

    deviceDesc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            fprintf(stderr, "GpuDevice: device lost (reason %d): %.*s\n",
                    (int)reason, (int)msg.length, msg.data);
        });
    deviceDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            fprintf(stderr, "GpuDevice: WebGPU error (type %d): %.*s\n",
                    (int)type, (int)msg.length, msg.data);
        });

    wgpu::Future future = mAdapter.RequestDevice(
        &deviceDesc,
        wgpu::CallbackMode::WaitAnyOnly,
        [&device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView msg) {
            if (status != wgpu::RequestDeviceStatus::Success) {
                fprintf(stderr, "GpuDevice: device request failed: %.*s\n",
                        (int)msg.length, msg.data);
                return;
            }
            device = std::move(result);
        });
    mInstance.WaitAny(future, UINT64_MAX);

    if (!device) {
        fprintf(stderr, "GpuDevice: failed to create WebGPU device\n");
        return false;
    }

    mDevice = std::move(device);
    mQueue = mDevice.GetQueue();
    return true;
}

bool GpuDevice::InitWindow(const GpuDeviceDesc& desc) {
    if (!glfwInit()) {
        fprintf(stderr, "GpuDevice: failed to initialize GLFW\n");
        return false;
    }

    // No OpenGL context — we're using WebGPU
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    mWindow = glfwCreateWindow(desc.width, desc.height, desc.title, nullptr, nullptr);
    if (!mWindow) {
        fprintf(stderr, "GpuDevice: failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }

    // Store this pointer for resize callback
    glfwSetWindowUserPointer(mWindow, this);
    // On macOS, Dawn's Metal surface uses window points, not framebuffer pixels.
    // Use window size callback to match. On Linux, framebuffer == window size.
#ifdef __APPLE__
    glfwSetWindowSizeCallback(mWindow, [](GLFWwindow* win, int w, int h) {
#else
    glfwSetFramebufferSizeCallback(mWindow, [](GLFWwindow* win, int w, int h) {
#endif
        auto* self = static_cast<GpuDevice*>(glfwGetWindowUserPointer(win));
        if (self && w > 0 && h > 0) {
            self->ResizeSurface(w, h);
        }
    });

    return true;
}

bool GpuDevice::InitSurface() {
    // Create surface from GLFW window using Dawn's native glfw integration
    // Dawn provides glfw::CreateSurfaceForGLFW, but it's simpler to do it manually
    wgpu::SurfaceDescriptor surfDesc{};

#if defined(__linux__)
    // Try Wayland first, fall back to X11
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        wgpu::SurfaceSourceWaylandSurface waylandDesc{};
        waylandDesc.display = glfwGetWaylandDisplay();
        waylandDesc.surface = glfwGetWaylandWindow(mWindow);
        surfDesc.nextInChain = &waylandDesc;
        mSurface = mInstance.CreateSurface(&surfDesc);
    } else {
        wgpu::SurfaceSourceXlibWindow x11Desc{};
        x11Desc.display = glfwGetX11Display();
        x11Desc.window = glfwGetX11Window(mWindow);
        surfDesc.nextInChain = &x11Desc;
        mSurface = mInstance.CreateSurface(&surfDesc);
    }
#elif defined(__APPLE__)
    // macOS: create CAMetalLayer via ObjC++ helper, pass to Dawn
    void* metalLayer = CreateMetalLayerForWindow(glfwGetCocoaWindow(mWindow));
    wgpu::SurfaceSourceMetalLayer metalDesc{};
    metalDesc.layer = metalLayer;
    surfDesc.nextInChain = &metalDesc;
    mSurface = mInstance.CreateSurface(&surfDesc);
#else
    #error "Unsupported platform"
#endif

    if (!mSurface) {
        fprintf(stderr, "GpuDevice: failed to create surface\n");
        return false;
    }
    return true;
}

static bool IsLinearFormat(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::BGRA8Unorm ||
           f == wgpu::TextureFormat::RGBA8Unorm;
}

void GpuDevice::ConfigureSurface() {
    // Query preferred format — prefer linear (non-sRGB) for gamma-space rendering
    // Xbox 360 does all lighting in gamma space, so we match that behavior
    wgpu::SurfaceCapabilities caps;
    mSurface.GetCapabilities(mAdapter, &caps);
    if (caps.formatCount > 0) {
        mSurfaceFormat = caps.formats[0];
        // Prefer a linear (non-sRGB) format to match Xbox 360 gamma-space rendering
        for (size_t i = 0; i < caps.formatCount; i++) {
            if (IsLinearFormat(caps.formats[i])) {
                mSurfaceFormat = caps.formats[i];
                break;
            }
        }
    }

    wgpu::SurfaceConfiguration config{};
    config.device = mDevice;
    config.format = mSurfaceFormat;
    config.width = mWidth;
    config.height = mHeight;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.presentMode = wgpu::PresentMode::Fifo; // VSync
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    mSurface.Configure(&config);
}

void GpuDevice::ResizeSurface(int width, int height) {
    if (width <= 0 || height <= 0) return;
    mWidth = width;
    mHeight = height;
    if (mSurface) {
        ConfigureSurface();
    }
}

wgpu::TextureView GpuDevice::AcquireNextFrame() {
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
    mSurface.Present();
    mInstance.ProcessEvents();
}

wgpu::TextureView GpuDevice::AcquireHeadlessFrame() {
    if (!mHeadlessTex) {
        wgpu::TextureDescriptor texDesc{};
        texDesc.label = "HeadlessTarget";
        texDesc.size = {(uint32_t)mWidth, (uint32_t)mHeight, 1};
        texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
        texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
        mHeadlessTex = mDevice.CreateTexture(&texDesc);
        mHeadlessView = mHeadlessTex.CreateView();
        mSurfaceFormat = wgpu::TextureFormat::RGBA8Unorm;
    }
    return mHeadlessView;
}

bool GpuDevice::ReadbackHeadlessFrame(uint8_t* outPixels, size_t outSize) {
    if (!mHeadlessTex) return false;

    uint32_t bytesPerRow = (uint32_t)mWidth * 4;
    uint32_t alignedBytesPerRow = (bytesPerRow + 255) & ~255u;
    size_t bufSize = alignedBytesPerRow * (uint32_t)mHeight;

    wgpu::BufferDescriptor readbackDesc{};
    readbackDesc.label = "HeadlessReadback";
    readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readbackDesc.size = bufSize;
    wgpu::Buffer readbackBuf = mDevice.CreateBuffer(&readbackDesc);

    wgpu::CommandEncoder encoder = mDevice.CreateCommandEncoder();

    wgpu::TexelCopyTextureInfo src{};
    src.texture = mHeadlessTex;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readbackBuf;
    dst.layout.bytesPerRow = alignedBytesPerRow;
    dst.layout.rowsPerImage = mHeight;
    wgpu::Extent3D copySize = {(uint32_t)mWidth, (uint32_t)mHeight, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &copySize);

    wgpu::CommandBuffer cmd = encoder.Finish();
    mQueue.Submit(1, &cmd);

    bool mapSuccess = false;
    mInstance.WaitAny(
        readbackBuf.MapAsync(
            wgpu::MapMode::Read, 0, bufSize,
            wgpu::CallbackMode::WaitAnyOnly,
            [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                mapSuccess = (status == wgpu::MapAsyncStatus::Success);
            }),
        UINT64_MAX);

    if (!mapSuccess) return false;

    const uint8_t* mapped = static_cast<const uint8_t*>(
        readbackBuf.GetConstMappedRange(0, bufSize));

    // Copy with potential row stride adjustment
    for (int y = 0; y < mHeight; y++) {
        size_t srcOff = y * alignedBytesPerRow;
        size_t dstOff = y * bytesPerRow;
        if (dstOff + bytesPerRow <= outSize) {
            memcpy(outPixels + dstOff, mapped + srcOff, bytesPerRow);
        }
    }

    readbackBuf.Unmap();
    return true;
}

bool GpuDevice::ShouldClose() const {
    return mWindow && glfwWindowShouldClose(mWindow);
}

void GpuDevice::PollEvents() {
    glfwPollEvents();
}

wgpu::Sampler GpuDevice::GetSampler(const SamplerDesc& desc) {
    auto it = mSamplerCache.find(desc);
    if (it != mSamplerCache.end()) {
        return it->second;
    }

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
