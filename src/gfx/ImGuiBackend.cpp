#include "gfx/ImGuiBackend.h"

#include <imgui.h>
#include <imgui_internal.h>  // InputEventsQueue: the frame mapping below
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>
#include <GLFW/glfw3.h>

#include "gfx/GpuDevice.h"

#include <cfloat>
#include <cmath>

static bool sInitialized = false;
static bool sHasWindow = false;
static GLFWwindow* sWindow = nullptr;
static int sMappedQueuedEvents = 0;

void ImGuiBackend::Init(GLFWwindow* window, wgpu::Device device, wgpu::TextureFormat surfaceFmt) {
    if (sInitialized) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // don't save imgui.ini

    // Query DPI scale from GLFW (Wayland/X11 content scale)
    float dpiScale = 1.0f;
    if (window) {
        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        dpiScale = yscale > 0.0f ? yscale : 1.0f;
    }

    ImGui::StyleColorsDark();

    // Scale widget sizes for readability (1.2x base * DPI)
    float uiScale = 1.2f * dpiScale;
    ImGui::GetStyle().ScaleAllSizes(uiScale);

    // Rasterize font at native pixel size to avoid blurry runtime scaling.
    // Default font is 13px; load at 13 * uiScale so the atlas has enough
    // texels for crisp rendering, then set FontGlobalScale = 1/dpiScale so
    // ImGui geometry stays in screen-coordinate space.
    float fontPx = std::round(13.0f * uiScale);
    ImFontConfig fontCfg;
    fontCfg.SizePixels = fontPx;
    fontCfg.OversampleH = 1; // no need for oversampling at native res
    fontCfg.OversampleV = 1;
    io.Fonts->AddFontDefault(&fontCfg);
    io.FontGlobalScale = 1.0f / dpiScale;

    // GLFW backend — install_callbacks=true chains to existing callbacks
    // Skip in headless mode (window == nullptr) to avoid GLFW null-window asserts
    sHasWindow = (window != nullptr);
    sWindow = window;
    if (sHasWindow) {
        ImGui_ImplGlfw_InitForOther(window, true);
    }

    // WebGPU backend
    ImGui_ImplWGPU_InitInfo wgpuInfo{};
    wgpuInfo.Device = device.Get();
    wgpuInfo.RenderTargetFormat = static_cast<WGPUTextureFormat>(surfaceFmt);
    wgpuInfo.DepthStencilFormat = WGPUTextureFormat_Undefined;
    wgpuInfo.NumFramesInFlight = 3;
    ImGui_ImplWGPU_Init(&wgpuInfo);

    sInitialized = true;
}

void ImGuiBackend::NewFrame() {
    if (!sInitialized) return;
    ImGui_ImplWGPU_NewFrame();
    if (sHasWindow) {
        ImGui_ImplGlfw_NewFrame();
        // A fixed-size frame scaled into the window (dc3 PLAN H-18): ImGui is
        // drawn into the frame, so it lays out at the frame's size and the
        // cursor is mapped from the window into it (through the bars and the
        // scale). GpuDevice is the window's user pointer.
        ImGuiContext& g = *GImGui;
        if (auto* gpu = sWindow ? static_cast<GpuDevice*>(glfwGetWindowUserPointer(sWindow)) : nullptr;
            gpu && gpu->HasFixedFrame()) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)gpu->WindowWidth(), (float)gpu->WindowHeight());
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            // Events ImGui left queued last frame (input trickling) are at the
            // front and were mapped then.
            for (int i = sMappedQueuedEvents; i < g.InputEventsQueue.Size; i++) {
                ImGuiInputEvent& e = g.InputEventsQueue[i];
                if (e.Type != ImGuiInputEventType_MousePos || e.MousePos.PosX == -FLT_MAX) continue;
                double fx = 0, fy = 0;
                gpu->WindowToFrame(e.MousePos.PosX, e.MousePos.PosY, fx, fy);
                e.MousePos.PosX = (float)fx;
                e.MousePos.PosY = (float)fy;
            }
        }
    }
    ImGui::NewFrame();
    sMappedQueuedEvents = GImGui->InputEventsQueue.Size;
}

void ImGuiBackend::Render(wgpu::RenderPassEncoder& pass) {
    if (!sInitialized) return;
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
}

void ImGuiBackend::Shutdown() {
    if (!sInitialized) return;
    ImGui_ImplWGPU_Shutdown();
    if (sHasWindow) {
        ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext();
    sInitialized = false;
}

bool ImGuiBackend::WantCaptureMouse() {
    if (!sInitialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiBackend::WantCaptureKeyboard() {
    if (!sInitialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}
