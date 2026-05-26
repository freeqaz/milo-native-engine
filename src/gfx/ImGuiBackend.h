#pragma once

#include <webgpu/webgpu_cpp.h>

struct GLFWwindow;

namespace ImGuiBackend {

// Initialize ImGui with GLFW + WebGPU (Dawn). Call after GpuDevice::Init().
void Init(GLFWwindow* window, wgpu::Device device, wgpu::TextureFormat surfaceFmt);

// Start a new ImGui frame. Call once per frame before building UI.
void NewFrame();

// Render ImGui draw data into the given render pass.
void Render(wgpu::RenderPassEncoder& pass);

// Shut down ImGui backends and destroy context.
void Shutdown();

// Returns true if ImGui wants to capture mouse input this frame.
bool WantCaptureMouse();

// Returns true if ImGui wants to capture keyboard input this frame.
bool WantCaptureKeyboard();

} // namespace ImGuiBackend
