#include "platform/DebugPanel.h"
#include "platform/NativeSettings.h"
#include <imgui.h>

static bool sVisible = false;

void DebugPanel::Init() {}

void DebugPanel::Draw() {
    if (!sVisible) return;

    NativeSettings &s = NativeSettings::Get();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Camera Debug (~)", &sVisible)) {
        ImGui::End();
        return;
    }

    // Camera Blend
    if (ImGui::CollapsingHeader("Camera Blend", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Blend", &s.cameraBlend);
        ImGui::SliderFloat("Same-cat frames", &s.blendFramesSame, 0.0f, 60.0f, "%.1f");
        ImGui::SliderFloat("Cross-cat frames", &s.blendFramesCross, 0.0f, 60.0f, "%.1f");
    }

    // FOV
    if (ImGui::CollapsingHeader("FOV")) {
        ImGui::SliderFloat("FOV Scale", &s.fovScale, 0.5f, 2.0f, "%.3f");
        if (ImGui::Button("Reset FOV")) s.fovScale = 1.0f;
    }

    // Near/Far Plane
    if (ImGui::CollapsingHeader("Clip Planes")) {
        ImGui::DragFloat("Near Override", &s.nearPlaneOverride, 0.01f, -1.0f, 100.0f, "%.2f");
        ImGui::DragFloat("Far Override", &s.farPlaneOverride, 1.0f, -1.0f, 10000.0f, "%.0f");
        ImGui::TextDisabled("-1 = use per-camera defaults");
    }

    // Aspect Ratio
    if (ImGui::CollapsingHeader("Aspect Ratio")) {
        ImGui::DragFloat("Aspect Override", &s.aspectOverride, 0.01f, -1.0f, 3.0f, "%.3f");
        ImGui::TextDisabled("-1 = auto from window size");
    }

    // Camera Offsets
    if (ImGui::CollapsingHeader("Camera Offset")) {
        ImGui::SliderFloat("Forward", &s.camForwardOffset, -5.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Height", &s.camHeightOffset, -5.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Lateral", &s.camLateralOffset, -5.0f, 5.0f, "%.2f");
        if (ImGui::Button("Reset Offsets")) {
            s.camForwardOffset = 0.0f;
            s.camHeightOffset = 0.0f;
            s.camLateralOffset = 0.0f;
        }
    }

    // Debug
    if (ImGui::CollapsingHeader("Debug")) {
        ImGui::Checkbox("Camera Debug Log", &s.cameraDebug);
    }

    ImGui::End();
}

bool DebugPanel::IsVisible() { return sVisible; }
void DebugPanel::Toggle() { sVisible = !sVisible; }
void DebugPanel::SetVisible(bool v) { sVisible = v; }
