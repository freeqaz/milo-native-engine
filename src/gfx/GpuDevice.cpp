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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool ValueTruthy(const char* v) {
    if (!v || !*v) return false;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' ||
             ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
}

// MILO_SURFACE_FAULT (test hook, dc3 PLAN H-33): make the window's surface
// fail the way an unusable one does, so the handling can be tested on a box
// whose surface works.
//   init      every window's surface probe reports unusable -> Init fails
//             with GpuInitFailure::SurfaceUnusable
//   init-once the first probe reports unusable; the fallback then runs once
//             even on X11 (a second window on the same window system), so the
//             teardown and re-init path runs without a Wayland compositor
//   acquire   GetCurrentTexture is treated as failing on every frame
enum SurfaceFault { kFaultNone = 0, kFaultInit, kFaultInitOnce, kFaultAcquire };

int ReadSurfaceFault() {
    const char* v = getenv("MILO_SURFACE_FAULT");
    if (!v || !*v) return kFaultNone;
    if (!strcmp(v, "init")) return kFaultInit;
    if (!strcmp(v, "init-once")) return kFaultInitOnce;
    if (!strcmp(v, "acquire")) return kFaultAcquire;
    fprintf(stderr, "GpuDevice: MILO_SURFACE_FAULT=%s not understood (init, init-once, acquire); ignored\n", v);
    return kFaultNone;
}

const char* AcquireStatusName(wgpu::SurfaceGetCurrentTextureStatus s) {
    switch (s) {
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal: return "SuccessOptimal";
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal: return "SuccessSuboptimal";
    case wgpu::SurfaceGetCurrentTextureStatus::Timeout: return "Timeout";
    case wgpu::SurfaceGetCurrentTextureStatus::Outdated: return "Outdated";
    case wgpu::SurfaceGetCurrentTextureStatus::Lost: return "Lost";
    case wgpu::SurfaceGetCurrentTextureStatus::Error: return "Error";
    default: return "unknown";
    }
}

#if defined(__linux__)
const char* GlfwPlatformName(int platform) {
    switch (platform) {
    case GLFW_PLATFORM_X11: return "x11";
    case GLFW_PLATFORM_WAYLAND: return "wayland";
    case GLFW_ANY_PLATFORM: return "any";
    default: return "other";
    }
}
#endif

}  // namespace

GpuDevice::~GpuDevice() {
    Shutdown();
}

bool GpuDevice::Init(const GpuDeviceDesc& desc) {
    mHeadless = desc.headless;
    mWidth = desc.width;
    mHeight = desc.height;
    mSurfaceWidth = desc.width;
    mSurfaceHeight = desc.height;
    mInitFailure = GpuInitFailure::NoFailure;
    mSurfaceFault = mHeadless ? kFaultNone : ReadSurfaceFault();

    if (!InitInstance()) {
        mInitFailure = GpuInitFailure::NoAdapter;
        return false;
    }
    if (!mHeadless) {
        // Window, surface and adapter together: the surface is needed before
        // the adapter (compatibleSurface), and whether it can be presented
        // to is only known once there is an adapter.
        if (!InitPresentation(desc)) return false;
    } else if (!InitAdapter()) {
        mInitFailure = GpuInitFailure::NoAdapter;
        return false;
    }
    if (!InitDevice()) {
        mInitFailure = GpuInitFailure::NoDevice;
        DestroyWindow();
        return false;
    }

    if (!mHeadless) {
        ConfigureSurface();
        if (desc.frameWidth > 0 && desc.frameHeight > 0) {
            mWidth = desc.frameWidth;
            mHeight = desc.frameHeight;
            CreateFrameTexture();
        } else {
            mWidth = mSurfaceWidth;
            mHeight = mSurfaceHeight;
        }
        printf("GpuDevice: initialized (%dx%d, windowed on %s, surface %dx%d%s)\n", mWidth,
               mHeight, WindowSystemName(), mSurfaceWidth, mSurfaceHeight,
               mFrameTex ? ", frame scaled to fit" : "");
    } else {
        printf("GpuDevice: initialized (%dx%d, headless)\n", mWidth, mHeight);
    }
    return true;
}

// One window system after another until a window's surface can be presented
// to (dc3 PLAN H-33). GLFW picks Wayland when WAYLAND_DISPLAY is set, and a
// Dawn built without Wayland support (the prebuilt one: its library has
// vkCreateXlibSurfaceKHR and no vkCreateWaylandSurfaceKHR) rejects the
// Wayland surface descriptor ("Unsupported sType"): the surface is an error
// object, every GetCurrentTexture fails at once, and the loop drew nothing at
// 6,000+ fps. Such a surface is now found here, and the window re-opened
// through XWayland when that is there.
bool GpuDevice::InitPresentation(const GpuDeviceDesc& desc) {
    int attempts[2] = {GLFW_ANY_PLATFORM, 0};
    int numAttempts = 1;
#if defined(__linux__)
    if (const char* p = getenv("MILO_GLFW_PLATFORM"); p && *p) {
        if (!strcmp(p, "x11")) attempts[0] = GLFW_PLATFORM_X11;
        else if (!strcmp(p, "wayland")) attempts[0] = GLFW_PLATFORM_WAYLAND;
        else if (strcmp(p, "any") != 0)
            fprintf(stderr, "GpuDevice: MILO_GLFW_PLATFORM=%s not understood (x11, wayland, any); ignored\n", p);
    }
#endif
    std::string lastWhy;
    bool anyWindow = false;
    for (int i = 0; i < numAttempts; i++) {
        if (!InitWindow(desc, attempts[i])) continue;
        anyWindow = true;
        std::string why;
        if (!InitSurface()) {
            why = "the surface could not be created";
        } else if (!InitAdapter()) {
            why = "no GPU adapter can present to it";
        } else if (SurfaceUsable(why)) {
            return true;
        }
        lastWhy = why;
        const char* system = WindowSystemName();
        int fallback = 0;
#if defined(__linux__)
        const int platform = glfwGetPlatform();
        if (i + 1 == numAttempts &&
            (platform != GLFW_PLATFORM_X11 || mSurfaceFault == kFaultInitOnce) &&
            glfwPlatformSupported(GLFW_PLATFORM_X11)) {
            fallback = GLFW_PLATFORM_X11;
        }
        if (mSurfaceFault == kFaultInitOnce) mSurfaceFault = kFaultNone;
#endif
        fprintf(stderr, "GpuDevice: the %s window cannot be presented to: %s%s\n", system,
                why.c_str(),
                fallback ? "; opening it again through X11 (XWayland)" : "");
        DestroyWindow();
        if (fallback) attempts[numAttempts++] = fallback;
    }
    if (!anyWindow) {
        mInitFailure = GpuInitFailure::NoWindow;
        return false;
    }
    mInitFailure = GpuInitFailure::SurfaceUnusable;
    mSurfaceFailureReason = lastWhy;
    return false;
}

// Whether the surface can be configured for this adapter: Dawn's error
// surface (an unsupported window system) fails GetCapabilities.
bool GpuDevice::SurfaceUsable(std::string& why) {
    if (mSurfaceFault == kFaultInit || mSurfaceFault == kFaultInitOnce) {
        why = "MILO_SURFACE_FAULT says so (test hook)";
        return false;
    }
    wgpu::SurfaceCapabilities caps;
    if (!mSurface.GetCapabilities(mAdapter, &caps)) {
        why = std::string("WebGPU has no surface for this window (GetCapabilities failed; this "
                          "Dawn build may not support the ") +
              WindowSystemName() + " window system)";
        return false;
    }
    if (caps.formatCount == 0) {
        why = "the surface offers no texture format";
        return false;
    }
    return true;
}

void GpuDevice::DestroyWindow() {
    mFrameView = nullptr;
    mFrameTex = nullptr;
    mSurfaceTex = nullptr;
    mSurface = nullptr;
    mAdapter = nullptr;
    if (mWindow) {
        glfwDestroyWindow(mWindow);
        mWindow = nullptr;
        glfwTerminate();
    }
}

const char* GpuDevice::WindowSystemName() const {
#if defined(__linux__)
    if (!mWindow) return "no";
    return GlfwPlatformName(glfwGetPlatform());
#elif defined(__APPLE__)
    return "cocoa";
#else
    return "other";
#endif
}

void GpuDevice::Shutdown() {
    mSamplerCache.clear();
    mHeadlessView = nullptr;
    mHeadlessTex = nullptr;
    mPresentScale = PresentScale();
    mFrameView = nullptr;
    mFrameTex = nullptr;
    mSurfaceTex = nullptr;
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

// Backend object labels: MILO_GPU_LABELS=1/0 decides; unset, they are on when
// the environment loads a Vulkan capture/debug layer (GFXReconstruct and the
// validation layers through VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE,
// RenderDoc through ENABLE_VULKAN_RENDERDOC_CAPTURE), so a capture keeps its
// object names without anyone having to remember the flag.
bool GpuLabelsWanted() {
    if (const char* v = getenv("MILO_GPU_LABELS"); v && *v)
        return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' ||
                 ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
    // Read, not set: these belong to the Vulkan loader and RenderDoc.
    auto set = [](const char* v) { return v && *v && strcmp(v, "0") != 0; };
    return set(getenv("VK_INSTANCE_LAYERS")) || set(getenv("VK_LOADER_LAYERS_ENABLE")) ||
           set(getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE"));
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

    // Dawn toggle that propagates WebGPU labels to VK_EXT_debug_utils (object
    // names in RenderDoc, GFXReconstruct, validation layers). It is a debugging
    // aid with a per-object cost: every Vulkan object Dawn creates -- each
    // frame's bind groups, command buffers and staging buffers included -- gets
    // an ostringstream-built name and a vkSetDebugUtilsObjectNameEXT call.
    // Measured on DC3 (headless, RTX 3090): 5-8% of the main thread in menus
    // and gameplay. So it is on only when a capture/debug layer is being
    // loaded, or when asked: MILO_GPU_LABELS=1 forces it on, =0 off.
    wgpu::DawnTogglesDescriptor toggles{};
    static const char* kEnabledToggles[] = {"use_user_defined_labels_in_backend"};
    if (GpuLabelsWanted()) {
        toggles.enabledToggleCount = 1;
        toggles.enabledToggles = kEnabledToggles;
        deviceDesc.nextInChain = &toggles;
        printf("GpuDevice: backend object labels ON (MILO_GPU_LABELS / a Vulkan layer is set)\n");
    }

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

bool GpuDevice::InitWindow(const GpuDeviceDesc& desc, int platform) {
#if defined(__linux__)
    glfwInitHint(GLFW_PLATFORM, platform);
#else
    (void)platform;
#endif
    if (!glfwInit()) {
        fprintf(stderr, "GpuDevice: failed to initialize GLFW%s\n",
#if defined(__linux__)
                platform == GLFW_ANY_PLATFORM ? "" : (platform == GLFW_PLATFORM_X11 ? " on X11" : " on Wayland")
#else
                ""
#endif
        );
        return false;
    }

    // No OpenGL context — we're using WebGPU
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // HiDPI. X11 has no compositor scaling: the window is sized by the
    // monitor's content scale (Xft.dpi), so 1280x720 at 2x opens 2560x1440
    // pixels. Wayland and macOS scale the framebuffer instead (GLFW's default
    // GLFW_SCALE_FRAMEBUFFER): the window keeps its size in screen
    // coordinates and the surface is sized from the framebuffer below.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    // MILO_FULLSCREEN=1: fullscreen on the primary monitor at its desktop
    // mode (no mode switch), for a player (dc3 scripts/play.sh --fullscreen).
    // MILO_WIDTH/HEIGHT are then the size the window returns to on F11.
    GLFWmonitor* monitor = nullptr;
    int width = desc.width, height = desc.height;
    if (desc.fullscreen || ValueTruthy(getenv("MILO_FULLSCREEN"))) {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (mode) {
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            width = mode->width;
            height = mode->height;
            const char* name = glfwGetMonitorName(monitor);
            printf("GpuDevice: fullscreen %dx%d@%d on %s\n", width, height, mode->refreshRate,
                   name ? name : "the primary monitor");
        } else {
            monitor = nullptr;
            fprintf(stderr, "GpuDevice: fullscreen: no monitor to fill, opening a window\n");
        }
    }

    mWindow = glfwCreateWindow(width, height, desc.title, monitor, nullptr);
    if (!mWindow) {
        fprintf(stderr, "GpuDevice: failed to create GLFW window\n");
        glfwTerminate();
        return false;
    }
    // Where F11 returns a window that started fullscreen.
    mWindowedX = 64;
    mWindowedY = 64;
    mWindowedW = desc.width;
    mWindowedH = desc.height;

    QuerySurfaceSize(mSurfaceWidth, mSurfaceHeight);
    if (mSurfaceWidth <= 0 || mSurfaceHeight <= 0) {
        mSurfaceWidth = width;
        mSurfaceHeight = height;
    }

    // The surface follows the window: AcquireNextFrame re-reads its size each
    // frame and reconfigures once when it moved (a resize, a toggle, a HiDPI
    // scale change), never once per resize event mid-drag.
    glfwSetWindowUserPointer(mWindow, this);
    mPrevKeyCallback = glfwSetKeyCallback(mWindow, KeyCallback);
    return true;
}

// The surface's size in pixels. On macOS Dawn's Metal surface uses window
// points, not framebuffer pixels; elsewhere it is the framebuffer (on Wayland
// with a scale, larger than the window's size in screen coordinates).
void GpuDevice::QuerySurfaceSize(int& w, int& h) const {
    w = h = 0;
    if (!mWindow) return;
#ifdef __APPLE__
    glfwGetWindowSize(mWindow, &w, &h);
#else
    glfwGetFramebufferSize(mWindow, &w, &h);
#endif
}

// F11, or Alt+Enter: toggle fullscreen. Chained, so a later key callback
// (the keyboard, ImGui) that chains in turn still sees every key. The keys
// also still reach the keyboard-as-pad map, which reads key state, and with
// Alt held that map reports no buttons, so Alt+Enter is never also A.
void GpuDevice::KeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods) {
    auto* self = static_cast<GpuDevice*>(glfwGetWindowUserPointer(win));
    if (self && action == GLFW_PRESS) {
        const int other = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_SUPER;
        if ((key == GLFW_KEY_F11 && !(mods & (other | GLFW_MOD_ALT))) ||
            ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) && (mods & GLFW_MOD_ALT) &&
             !(mods & other))) {
            self->mFullscreenToggleRequested = true;
        }
    }
    if (self && self->mPrevKeyCallback) self->mPrevKeyCallback(win, key, scancode, action, mods);
}

bool GpuDevice::IsFullscreen() const {
    return mWindow && glfwGetWindowMonitor(mWindow) != nullptr;
}

void GpuDevice::SetFullscreen(bool on) {
    if (!mWindow || on == IsFullscreen()) return;
    if (on) {
        // The monitor holding the window's centre, else the primary one.
        glfwGetWindowPos(mWindow, &mWindowedX, &mWindowedY);
        glfwGetWindowSize(mWindow, &mWindowedW, &mWindowedH);
        const int cx = mWindowedX + mWindowedW / 2, cy = mWindowedY + mWindowedH / 2;
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        GLFWmonitor* monitor = nullptr;
        for (int i = 0; i < count && !monitor; i++) {
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode* m = glfwGetVideoMode(monitors[i]);
            if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height)
                monitor = monitors[i];
        }
        if (!monitor) monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!mode) {
            fprintf(stderr, "GpuDevice: fullscreen: no monitor to fill\n");
            return;
        }
        glfwSetWindowMonitor(mWindow, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        const char* name = glfwGetMonitorName(monitor);
        printf("GpuDevice: fullscreen %dx%d@%d on %s\n", mode->width, mode->height,
               mode->refreshRate, name ? name : "the monitor");
    } else {
        glfwSetWindowMonitor(mWindow, nullptr, mWindowedX, mWindowedY,
                             mWindowedW > 0 ? mWindowedW : 1280,
                             mWindowedH > 0 ? mWindowedH : 720, 0);
        printf("GpuDevice: windowed %dx%d\n", mWindowedW, mWindowedH);
    }
    fflush(stdout);
}

void GpuDevice::ApplyPendingFullscreenToggle() {
    if (!mFullscreenToggleRequested) return;
    mFullscreenToggleRequested = false;
    SetFullscreen(!IsFullscreen());
}

bool GpuDevice::WindowToFrame(double winX, double winY, double& frameX, double& frameY) const {
    frameX = winX;
    frameY = winY;
    if (!mWindow) return true;
    int ww = 0, wh = 0;
    glfwGetWindowSize(mWindow, &ww, &wh);
    if (ww <= 0 || wh <= 0 || mSurfaceWidth <= 0 || mSurfaceHeight <= 0) return false;
    // Window coordinates -> surface pixels (HiDPI) -> frame pixels.
    const double sx = winX * mSurfaceWidth / ww, sy = winY * mSurfaceHeight / wh;
    if (!mFrameTex) {
        frameX = sx;
        frameY = sy;
        return true;
    }
    const PresentRect r = FitPresentRect(mWidth, mHeight, mSurfaceWidth, mSurfaceHeight);
    if (r.w <= 0 || r.h <= 0) return false;
    frameX = (sx - r.x) * mWidth / r.w;
    frameY = (sy - r.y) * mHeight / r.h;
    return frameX >= 0 && frameY >= 0 && frameX < mWidth && frameY < mHeight;
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
    config.width = mSurfaceWidth;
    config.height = mSurfaceHeight;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    // CopySrc where the surface offers it: a full-frame pass that reads the
    // finished frame (gfx/DisplayRamp) copies it out first.
    mSurfaceCopySrc = (caps.usages & wgpu::TextureUsage::CopySrc) != 0;
    if (mSurfaceCopySrc)
        config.usage = config.usage | wgpu::TextureUsage::CopySrc;
    config.presentMode = wgpu::PresentMode::Fifo; // VSync
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    // The old swapchain's textures are released here, not held across it.
    mSurfaceTex = nullptr;
    mSurface.Configure(&config);
    mSurfaceNeedsConfigure = false;
}

// The fixed-size frame (GpuDeviceDesc::frameWidth): drawn into, read by a
// full-frame pass over the finished frame (gfx/DisplayRamp copies it) and by
// readback, sampled by PresentScale. Same format as the surface, so every
// pipeline built for SurfaceFormat() draws into it unchanged.
void GpuDevice::CreateFrameTexture() {
    wgpu::TextureDescriptor td{};
    td.label = "FrameTarget";
    td.size = {(uint32_t)mWidth, (uint32_t)mHeight, 1};
    td.format = mSurfaceFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    mFrameTex = mDevice.CreateTexture(&td);
    mFrameView = mFrameTex.CreateView();
}

void GpuDevice::ResizeSurface(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == mSurfaceWidth && height == mSurfaceHeight && !mSurfaceNeedsConfigure) return;
    mSurfaceWidth = width;
    mSurfaceHeight = height;
    if (!mFrameTex) {
        mWidth = width;
        mHeight = height;
    }
    if (mSurface) {
        ConfigureSurface();
    }
}

wgpu::TextureView GpuDevice::AcquireNextFrame() {
    if (!mSurface) return nullptr;
    // Follow the window: one reconfigure when its size moved since the last
    // frame (a 0x0 framebuffer -- minimised -- is left alone).
    {
        int w = 0, h = 0;
        QuerySurfaceSize(w, h);
        if (w > 0 && h > 0 && (w != mSurfaceWidth || h != mSurfaceHeight || mSurfaceNeedsConfigure))
            ResizeSurface(w, h);
    }
    wgpu::SurfaceTexture surfTex;
    if (mSurfaceFault == kFaultAcquire) {
        surfTex.status = wgpu::SurfaceGetCurrentTextureStatus::Error;
    } else {
        mSurface.GetCurrentTexture(&surfTex);
    }
    if (surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
        surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        if (surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
            mSurfaceNeedsConfigure = true;  // reconfigured before the next frame
        if (mAcquireFailures > 0) {
            printf("GpuDevice: the surface gives frames again (after %lld failed)\n",
                   mAcquireFailures);
            mAcquireFailures = 0;
        }
        mEverAcquired = true;
        mSurfaceTex = surfTex.texture;
        return mFrameTex ? mFrameView : mSurfaceTex.CreateView();
    }

    // No frame (dc3 PLAN H-33). Never return at once: the caller's loop would
    // spin a core drawing nothing. A frame period of sleep holds it near the
    // display rate until the surface comes back (Outdated/Lost after a resize
    // or a mode change: reconfigured next time) or, if it never gave a frame
    // at all, until the caller gives up on SurfaceFailed().
    const double now = NowSeconds();
    if (mAcquireFailures++ == 0) mFirstFailureTime = now;
    if (surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::Outdated ||
        surfTex.status == wgpu::SurfaceGetCurrentTextureStatus::Lost)
        mSurfaceNeedsConfigure = true;
    if (mAcquireFailures == 1 || mAcquireFailures % 300 == 0) {
        fprintf(stderr,
                "GpuDevice: the %s window's surface gave no frame (%s; %lld in a row over %.1f s)\n",
                WindowSystemName(), AcquireStatusName(surfTex.status), mAcquireFailures,
                now - mFirstFailureTime);
    }
    if (!mEverAcquired && now - mFirstFailureTime >= kSurfaceGiveUpSeconds && !mSurfaceFailed) {
        mSurfaceFailed = true;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "the %s window's surface has not given a single frame in %.0f s "
                 "(%lld attempts, last status %s)",
                 WindowSystemName(), now - mFirstFailureTime, mAcquireFailures,
                 AcquireStatusName(surfTex.status));
        mSurfaceFailureReason = buf;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return nullptr;
}

void GpuDevice::PresentFrame() {
    if (!mSurface || !mSurfaceTex) return;
    if (mFrameTex) {
        wgpu::CommandEncoderDescriptor ed{};
        ed.label = "PresentScaleEncoder";
        wgpu::CommandEncoder enc = mDevice.CreateCommandEncoder(&ed);
        mPresentRect = mPresentScale.Encode(mDevice, enc, mFrameTex, mSurfaceTex.CreateView(),
                                            mSurfaceFormat, (int)mSurfaceTex.GetWidth(),
                                            (int)mSurfaceTex.GetHeight());
        wgpu::CommandBuffer cmd = enc.Finish();
        mQueue.Submit(1, &cmd);
    } else {
        mPresentRect = {0, 0, (int)mSurfaceTex.GetWidth(), (int)mSurfaceTex.GetHeight()};
    }
    mSurface.Present();
    mSurfaceTex = nullptr;
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
    return ReadbackTexture(mHeadlessTex, mWidth, mHeight, outPixels, outSize);
}

bool GpuDevice::ReadbackFrame(uint8_t* outPixels, size_t outSize) {
    if (mHeadless) return ReadbackHeadlessFrame(outPixels, outSize);
    // The owned frame (always CopySrc, and readable after present too), else
    // the swapchain texture while it is held.
    wgpu::Texture& tex = mFrameTex ? mFrameTex : mSurfaceTex;
    if (!tex || (!mFrameTex && !mSurfaceCopySrc)) return false;
    int w = (int)tex.GetWidth(), h = (int)tex.GetHeight();
    if ((size_t)w * h * 4 > outSize) return false;
    if (!ReadbackTexture(tex, w, h, outPixels, outSize)) return false;
    if (mSurfaceFormat == wgpu::TextureFormat::BGRA8Unorm ||
        mSurfaceFormat == wgpu::TextureFormat::BGRA8UnormSrgb) {
        for (size_t i = 0, n = (size_t)w * h * 4; i < n; i += 4) {
            uint8_t b = outPixels[i];
            outPixels[i] = outPixels[i + 2];
            outPixels[i + 2] = b;
        }
    }
    return true;
}

bool GpuDevice::ReadbackTexture(const wgpu::Texture& tex, int w, int h,
                                uint8_t* outPixels, size_t outSize) {
    if (!tex || w <= 0 || h <= 0) return false;

    uint32_t bytesPerRow = (uint32_t)w * 4;
    uint32_t alignedBytesPerRow = (bytesPerRow + 255) & ~255u;
    size_t bufSize = alignedBytesPerRow * (uint32_t)h;

    wgpu::BufferDescriptor readbackDesc{};
    readbackDesc.label = "TextureReadback";
    readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readbackDesc.size = bufSize;
    wgpu::Buffer readbackBuf = mDevice.CreateBuffer(&readbackDesc);

    wgpu::CommandEncoder encoder = mDevice.CreateCommandEncoder();

    wgpu::TexelCopyTextureInfo src{};
    src.texture = tex;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readbackBuf;
    dst.layout.bytesPerRow = alignedBytesPerRow;
    dst.layout.rowsPerImage = (uint32_t)h;
    wgpu::Extent3D copySize = {(uint32_t)w, (uint32_t)h, 1};
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
    for (int y = 0; y < h; y++) {
        size_t srcOff = y * alignedBytesPerRow;
        size_t dstOff = y * bytesPerRow;
        if (dstOff + bytesPerRow <= outSize) {
            memcpy(outPixels + dstOff, mapped + srcOff, bytesPerRow);
        }
    }

    readbackBuf.Unmap();
    return true;
}

void GpuDevice::WarmUp() {
    // Force the GPU driver's first-use / worker-thread machinery (command-pool
    // and descriptor-pool allocators, the driver's internal submission threads)
    // to spin up and FULLY SETTLE synchronously, before any *other* thread
    // (notably the miniaudio playback thread) starts up and contends with it.
    //
    // On some hosts (observed: NVIDIA proprietary Vulkan, driver 595.71.05) the
    // very first descriptor-pool allocation lazily spawns internal driver worker
    // threads; if the audio device's init/thread-start runs concurrently with
    // that first allocation, an NVIDIA driver worker thread faults (SIGSEGV deep
    // in libnvidia-eglcore, never in our code). App.cpp orders SynthInit()
    // (which starts the audio thread) immediately before the first
    // BeginDrawing()/EndDrawing(), so the two race. Calling WarmUp() from the
    // native synth init BEFORE the audio thread starts removes the overlap.
    if (!mDevice)
        return;

    // A trivial empty command buffer is enough to drive the queue's first
    // submission through the driver's threaded path.
    wgpu::CommandEncoder enc = mDevice.CreateCommandEncoder();
    wgpu::CommandBuffer cmd = enc.Finish();
    mQueue.Submit(1, &cmd);

    // Block until that submission is fully retired, so the driver's lazy
    // worker-thread spin-up is complete before we return.
    bool done = false;
    mInstance.WaitAny(
        mQueue.OnSubmittedWorkDone(
            wgpu::CallbackMode::WaitAnyOnly,
            [&done](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; }),
        UINT64_MAX);
    mInstance.ProcessEvents();
    (void)done;
}

bool GpuDevice::ShouldClose() const {
    return mWindow && glfwWindowShouldClose(mWindow);
}

void GpuDevice::PollEvents() {
    glfwPollEvents();
    ApplyPendingFullscreenToggle();
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
