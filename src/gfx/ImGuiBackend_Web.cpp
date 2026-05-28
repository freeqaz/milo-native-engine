// ImGui backend for Emscripten/web — replaces ImGuiBackend.cpp (GLFW-based).
// Uses Emscripten HTML5 API for keyboard/mouse input + imgui_impl_wgpu for rendering.
//
// Canvas selector is consumer-controlled via MILO_WEB_CANVAS_SELECTOR
// (default "#milo-canvas"; DC3 sets "#dc3-canvas", RB3 sets "#rb3-canvas").

#include "gfx/ImGuiBackend.h"

#include <imgui.h>
#include <imgui_impl_wgpu.h>
#include <emscripten/html5.h>
#include <cstring>

#include "platform/DebugPanel.h"

#ifndef MILO_WEB_CANVAS_SELECTOR
#define MILO_WEB_CANVAS_SELECTOR "#milo-canvas"
#endif

static bool sInitialized = false;

// Map DOM key codes to ImGui keys
static ImGuiKey DomKeyToImGui(const char *code) {
    if (strcmp(code, "Backquote") == 0) return ImGuiKey_GraveAccent;
    if (strcmp(code, "Tab") == 0) return ImGuiKey_Tab;
    if (strcmp(code, "ArrowLeft") == 0) return ImGuiKey_LeftArrow;
    if (strcmp(code, "ArrowRight") == 0) return ImGuiKey_RightArrow;
    if (strcmp(code, "ArrowUp") == 0) return ImGuiKey_UpArrow;
    if (strcmp(code, "ArrowDown") == 0) return ImGuiKey_DownArrow;
    if (strcmp(code, "Enter") == 0) return ImGuiKey_Enter;
    if (strcmp(code, "Escape") == 0) return ImGuiKey_Escape;
    if (strcmp(code, "Backspace") == 0) return ImGuiKey_Backspace;
    if (strcmp(code, "Space") == 0) return ImGuiKey_Space;
    if (strcmp(code, "Delete") == 0) return ImGuiKey_Delete;
    if (strcmp(code, "Home") == 0) return ImGuiKey_Home;
    if (strcmp(code, "End") == 0) return ImGuiKey_End;
    // Single-char keys (A-Z, 0-9)
    if (code[0] == 'K' && code[1] == 'e' && code[2] == 'y' && code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0')
        return (ImGuiKey)(ImGuiKey_A + (code[3] - 'A'));
    if (code[0] == 'D' && code[1] == 'i' && code[2] == 'g' && code[3] == 'i' && code[4] == 't' && code[5] >= '0' && code[5] <= '9' && code[6] == '\0')
        return (ImGuiKey)(ImGuiKey_0 + (code[5] - '0'));
    return ImGuiKey_None;
}

static EM_BOOL OnKeyDown(int, const EmscriptenKeyboardEvent *e, void *) {
    // Backtick toggles debug panel (handled before ImGui so it works even when panel is hidden)
    if (strcmp(e->code, "Backquote") == 0) {
        DebugPanel::Toggle();
        return EM_TRUE;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImGuiKey key = DomKeyToImGui(e->code);
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, true);
    return io.WantCaptureKeyboard ? EM_TRUE : EM_FALSE;
}

static EM_BOOL OnKeyUp(int, const EmscriptenKeyboardEvent *e, void *) {
    ImGuiIO &io = ImGui::GetIO();
    ImGuiKey key = DomKeyToImGui(e->code);
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, false);
    return io.WantCaptureKeyboard ? EM_TRUE : EM_FALSE;
}

static EM_BOOL OnMouseMove(int, const EmscriptenMouseEvent *e, void *) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((float)e->targetX, (float)e->targetY);
    return io.WantCaptureMouse ? EM_TRUE : EM_FALSE;
}

static EM_BOOL OnMouseDown(int, const EmscriptenMouseEvent *e, void *) {
    ImGuiIO &io = ImGui::GetIO();
    if (e->button <= 2)
        io.AddMouseButtonEvent(e->button, true);
    return io.WantCaptureMouse ? EM_TRUE : EM_FALSE;
}

static EM_BOOL OnMouseUp(int, const EmscriptenMouseEvent *e, void *) {
    ImGuiIO &io = ImGui::GetIO();
    if (e->button <= 2)
        io.AddMouseButtonEvent(e->button, false);
    return io.WantCaptureMouse ? EM_TRUE : EM_FALSE;
}

static EM_BOOL OnWheel(int, const EmscriptenWheelEvent *e, void *) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddMouseWheelEvent((float)-e->deltaX * 0.01f, (float)-e->deltaY * 0.01f);
    return io.WantCaptureMouse ? EM_TRUE : EM_FALSE;
}

void ImGuiBackend::Init(GLFWwindow *, wgpu::Device device, wgpu::TextureFormat surfaceFmt) {
    if (sInitialized) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.2f);
    io.FontGlobalScale = 1.2f;

    // WebGPU backend
    ImGui_ImplWGPU_InitInfo wgpuInfo{};
    wgpuInfo.Device = device.Get();
    wgpuInfo.RenderTargetFormat = static_cast<WGPUTextureFormat>(surfaceFmt);
    wgpuInfo.DepthStencilFormat = WGPUTextureFormat_Undefined;
    wgpuInfo.NumFramesInFlight = 3;
    ImGui_ImplWGPU_Init(&wgpuInfo);

    // Register Emscripten HTML5 input callbacks on the canvas
    const char *target = MILO_WEB_CANVAS_SELECTOR;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, OnKeyDown);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, OnKeyUp);
    emscripten_set_mousemove_callback(target, nullptr, EM_TRUE, OnMouseMove);
    emscripten_set_mousedown_callback(target, nullptr, EM_TRUE, OnMouseDown);
    emscripten_set_mouseup_callback(target, nullptr, EM_TRUE, OnMouseUp);
    emscripten_set_wheel_callback(target, nullptr, EM_TRUE, OnWheel);

    sInitialized = true;
}

void ImGuiBackend::NewFrame() {
    if (!sInitialized) return;
    ImGui_ImplWGPU_NewFrame();

    // Manually set display size from canvas (no GLFW to do this for us)
    int w, h;
    emscripten_get_canvas_element_size(MILO_WEB_CANVAS_SELECTOR, &w, &h);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    // DeltaTime
    static double lastTime = emscripten_get_now() / 1000.0;
    double now = emscripten_get_now() / 1000.0;
    io.DeltaTime = (float)(now - lastTime);
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
    lastTime = now;

    ImGui::NewFrame();
}

void ImGuiBackend::Render(wgpu::RenderPassEncoder &pass) {
    if (!sInitialized) return;
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
}

void ImGuiBackend::Shutdown() {
    if (!sInitialized) return;
    ImGui_ImplWGPU_Shutdown();
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
