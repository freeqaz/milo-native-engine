// WebGPU Device (Browser) — replaces GpuDevice.cpp for Emscripten builds.
// Uses emdawnwebgpu: same webgpu.h API but async init + canvas surface.
//
// Canvas selector is consumer-controlled via MILO_WEB_CANVAS_SELECTOR
// (default "#milo-canvas"; DC3 sets "#dc3-canvas", RB3 sets "#rb3-canvas").

#ifdef __EMSCRIPTEN__

#include "gfx/GpuDevice.h"
#include "gfx/GpuTimers.h"
#include "gfx/FrameCost.h"

#include <emscripten/emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/html5.h>

#include <cstdio>
#include <cstring>

#ifndef MILO_WEB_CANVAS_SELECTOR
#define MILO_WEB_CANVAS_SELECTOR "#milo-canvas"
#endif

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
    // Selector arrives as a UTF8 C-string ($0); JS slices the leading '#'.
    int canvasW = EM_ASM_INT({
        var sel = UTF8ToString($0);
        var id = sel.charAt(0) === '#' ? sel.substring(1) : sel;
        var c = document.getElementById(id);
        return c ? c.width : 0;
    }, MILO_WEB_CANVAS_SELECTOR);
    int canvasH = EM_ASM_INT({
        var sel = UTF8ToString($0);
        var id = sel.charAt(0) === '#' ? sel.substring(1) : sel;
        var c = document.getElementById(id);
        return c ? c.height : 0;
    }, MILO_WEB_CANVAS_SELECTOR);
    mWidth = (canvasW > 0) ? canvasW : desc.width;
    mHeight = (canvasH > 0) ? canvasH : desc.height;

    // A frame that follows its target (GpuDeviceDesc::outputFollowsTarget,
    // dc3's enhanced look): the canvas's buffer is the output -- the logical
    // shape fitted into the canvas's box in device pixels, held to the row
    // cap (WebOutputRowCap) -- and viewports, UI and post measures stay in the
    // logical size (LogicalWidth/Height), as on the desktop. The web never
    // supersamples (render scale 1: the browser's own scaling is the
    // present). The sizes are resolved once the adapter is known (the cap
    // depends on it), before the surface is first configured. A frame that
    // does not follow keeps the canvas's own buffer size, as before.
    mSizeDesc = WebFrameDesc(desc);
    mFollowsTarget = desc.outputFollowsTarget && desc.logicalWidth > 0 && desc.logicalHeight > 0;
    // The virtual resolution whether or not the frame follows (GpuDevice.cpp).
    const bool hasLogical = desc.logicalWidth > 0 && desc.logicalHeight > 0;
    mLogicalWidth = hasLogical ? desc.logicalWidth : 0;
    mLogicalHeight = hasLogical ? desc.logicalHeight : 0;

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

            // np-look56: the look's adapter class, from what the browser says
            // about its GPU (milo::ClassifyWebLookAdapter: the vendor string,
            // BC, fallback). emdawnwebgpu fills only the strings from the
            // page's GPUAdapterInfo; adapterType is CPU for a fallback adapter
            // and Unknown otherwise, vendorID 0, backendType WebGPU.
            // Look rules only (GpuDevice::LookClass()): nothing that sizes the
            // frame changes. mDiscreteGpu stays false on the web, the web
            // never supersamples (render scale 1), and the row cap stays
            // WebOutputRowCap's (texture-compression-bc).
            {
                wgpu::AdapterInfo info{};
                mAdapter.GetInfo(&info);
                mLookAdapter = milo::LookAdapterFacts{};
                mLookAdapter.web = true;
                mLookAdapter.backendName = "WebGPU";
                mLookAdapter.type = info.adapterType == wgpu::AdapterType::CPU ? milo::LookAdapterType::Cpu
                                                                               : milo::LookAdapterType::Unknown;
                mLookAdapter.webFallback = info.adapterType == wgpu::AdapterType::CPU;
                mLookAdapter.webHasBC = mHasBCCompression;
                milo::SetLookAdapterWebStrings(mLookAdapter, info.vendor.data, info.vendor.length,
                                               info.architecture.data, info.architecture.length);
            }

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
                    // MILO_GPU_TIMERS: not wired in the browser build (one log line).
                    GpuTimers::Get().NoteUnavailable(
                        "not implemented in the web build (no timestamp-query requested)");

                    // Create canvas surface
                    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
                    canvasDesc.selector = MILO_WEB_CANVAS_SELECTOR;

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
                    if (mFollowsTarget) {
                        // The cap is a heuristic on web (a browser does not
                        // say whether its adapter is discrete): print it.
                        mWebRowCap = WebOutputRowCap(mSizeDesc, mHasBCCompression);
                        mRowCap = mWebRowCap.rows;
                        char capText[96];
                        printf("[look] web output follows the canvas, %s\n",
                               DescribeRowCap(mWebRowCap, capText, sizeof(capText)));
                        int tw = 0, th = 0;
                        if (!CanvasTargetSize(tw, th)) {
                            tw = mWidth;
                            th = mHeight;
                        }
                        mCanvasSize.Take(tw, th);
                        FollowTarget(tw, th, true);
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

bool GpuDevice::CanvasSize(int& width, int& height) const {
    width = height = 0;
    if (emscripten_get_canvas_element_size(MILO_WEB_CANVAS_SELECTOR, &width, &height) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
        width = height = 0;
        return false;
    }
    return true;
}

// The canvas's box in device pixels, packed (CanvasTargetSize). EM_JS, not
// EM_ASM: the body has commas outside parentheses.
EM_JS(int, milo_web_canvas_target_packed, (const char* selector), {
    var p = (typeof window !== 'undefined') ? window.miloCanvasDevicePixels : null;
    var w = 0, h = 0;
    if (p && p.length === 2) {
        w = p[0] | 0;
        h = p[1] | 0;
    } else {
        var sel = UTF8ToString(selector);
        var c = document.getElementById(sel.charAt(0) === '#' ? sel.substring(1) : sel);
        if (!c) return 0;
        var r = c.getBoundingClientRect();
        var dpr = window.devicePixelRatio || 1;
        w = Math.round(r.width * dpr);
        h = Math.round(r.height * dpr);
    }
    if (w <= 0 || h <= 0 || w > 32767 || h > 65535) return 0;
    return w * 65536 + h;
});

// The canvas's box in device pixels: the page's ResizeObserver publishes it
// as window.miloCanvasDevicePixels = [w, h] (devicePixelContentBoxSize where
// the browser has it, exact; else its CSS box times devicePixelRatio). Without
// it, the CSS box times devicePixelRatio, here.
bool GpuDevice::CanvasTargetSize(int& w, int& h) const {
    // Packed as w * 65536 + h (w < 32768, h < 65536); 0 = no canvas or an empty box.
    const int packed = milo_web_canvas_target_packed(MILO_WEB_CANVAS_SELECTOR);
    if (packed <= 0) {
        w = h = 0;
        return false;
    }
    w = packed >> 16;
    h = packed & 0xffff;
    return true;
}

// Re-resolve a following frame's sizes for a target of targetW x targetH device
// pixels; when the drawn size moves (or at init), size the canvas's buffer to
// it and configure the surface, once.
bool GpuDevice::FollowTarget(int targetW, int targetH, bool atInit) {
    const FrameSizes s =
        ResolveFrameSizesWithCap(mSizeDesc, false, true, targetW, targetH, mRowCap);
    if (!atInit && s.drawnW == mWidth && s.drawnH == mHeight) return false;
    emscripten_set_canvas_element_size(MILO_WEB_CANVAS_SELECTOR, s.drawnW, s.drawnH);
    ApplyFrameSizes(s, atInit);
    if (!atInit) {
        if (mSurface && mDevice) ConfigureSurface();
        mFrameReconfigures++;
    }
    printf("GpuDevice: canvas box %dx%d device px -> buffer %dx%d%s (reconfigures %d)\n",
           targetW, targetH, s.drawnW, s.drawnH, atInit ? " at init" : "", mFrameReconfigures);
    EM_ASM({ window.miloFrameReconfigures = $0; }, mFrameReconfigures);
    return !atInit;
}

bool GpuDevice::FollowCanvas() {
    if (!mFollowsTarget || !mSurface || !mDevice) return false;
    int tw = 0, th = 0;
    // An empty box (a collapsed panel, display:none): keep the frame as it is.
    if (!CanvasTargetSize(tw, th)) return false;
    if (!mCanvasSize.Offer(tw, th, emscripten_get_now())) return false;
    return FollowTarget(tw, th, false);
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
    {
        framecost::Scope cost(framecost::kAcquire);
        mSurface.GetCurrentTexture(&surfTex);
    }
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

bool GpuDevice::ReadbackFrame(uint8_t*, size_t) {
    return false;
}

bool GpuDevice::ReadbackTexture(const wgpu::Texture&, int, int, uint8_t*, size_t) {
    return false;
}

bool GpuDevice::ShouldClose() const { return false; }

// The browser has no headless mode; requestAnimationFrame paces the frames.
void GpuDevice::EndHeadlessFrame() {}

bool GpuDevice::IsWindowFocused() const { return true; }

void GpuDevice::PollEvents() {
    if (mInstance) mInstance.ProcessEvents();
}

wgpu::Sampler GpuDevice::GetSampler(const SamplerDesc& desc) {
    auto it = mSamplerCache.find(desc);
    if (it != mSamplerCache.end()) return it->second;

    wgpu::SamplerDescriptor sd = desc.ToWgpu();

    wgpu::Sampler sampler = mDevice.CreateSampler(&sd);
    mSamplerCache[desc] = sampler;
    return sampler;
}

#endif // __EMSCRIPTEN__
