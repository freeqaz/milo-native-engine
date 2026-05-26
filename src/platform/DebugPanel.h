#pragma once

// ImGui-based debug overlay for dc3-native.
// Provides real-time sliders for NativeSettings camera parameters.
// Toggle with backtick (~) key or via DTA {profile_mgr toggle_debug_panel}.

namespace DebugPanel {

void Init();
void Draw();        // Call between ImGuiBackend::NewFrame() and ImGui::Render()
bool IsVisible();
void Toggle();
void SetVisible(bool v);

} // namespace DebugPanel
