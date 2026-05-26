#pragma once
#include <cstdlib>
#include <cstdio>

// Native-only runtime settings. These are enhancements or toggles that don't
// exist on Xbox 360 — the original build has no equivalent options.
// On by default where they improve the experience; togglable for fidelity.
//
// All camera/rendering parameters can be overridden via environment variables.
// Use these to fine-tune the native WebGPU renderer against the DX9 original.

struct NativeSettings {
    // --- Camera Blend ---
    // Xbox always does instant hard cuts (mBlendTime=0) because the DTA
    // pick_shot flow_command path is dead. This injects blend times.
    bool cameraBlend = false;
    float blendFramesSame = 10.0f;   // blend frames for same-category cuts
    float blendFramesCross = 15.0f;  // blend frames for cross-category cuts

    // --- FOV ---
    // Scale factor applied to all camera FOVs. 1.0 = original.
    // DX9 and WebGPU have slightly different projection conventions;
    // this lets you compensate if the framing looks too wide/narrow.
    float fovScale = 1.0f;

    // --- Near/Far Plane ---
    // Override near/far planes globally. -1 = use per-camera values.
    // Useful for debugging depth buffer precision issues.
    float nearPlaneOverride = -1.0f;
    float farPlaneOverride = -1.0f;

    // --- Aspect Ratio ---
    // Override aspect ratio. -1 = auto from window size.
    float aspectOverride = -1.0f;

    // --- Camera Position Offset ---
    // Offsets applied in view space after the camera transform.
    // Forward: positive = closer to subject, negative = farther away.
    // Height: positive = camera moves up, negative = down.
    // Lateral: positive = camera moves right, negative = left.
    float camForwardOffset = 0.0f;
    float camHeightOffset = 0.0f;
    float camLateralOffset = 0.0f;

    // --- Debug ---
    // Show camera debug overlay (FOV, position, near/far, shot name, blend state)
    bool cameraDebug = false;

    void Init() {
        // Parse all camera env vars
if (const char *v = getenv("MILO_CAM_FOV_SCALE"))
            fovScale = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_NEAR"))
            nearPlaneOverride = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_FAR"))
            farPlaneOverride = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_ASPECT"))
            aspectOverride = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_FORWARD"))
            camForwardOffset = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_HEIGHT"))
            camHeightOffset = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_LATERAL"))
            camLateralOffset = (float)atof(v);
        if (const char *v = getenv("MILO_CAM_DEBUG"))
            cameraDebug = atoi(v) != 0;

        // Log active overrides
        if (fovScale != 1.0f)
            fprintf(stderr, "[NativeSettings] FOV scale: %.3f\n", fovScale);
        if (nearPlaneOverride > 0)
            fprintf(stderr, "[NativeSettings] Near plane override: %.2f\n", nearPlaneOverride);
        if (farPlaneOverride > 0)
            fprintf(stderr, "[NativeSettings] Far plane override: %.2f\n", farPlaneOverride);
        if (aspectOverride > 0)
            fprintf(stderr, "[NativeSettings] Aspect override: %.3f\n", aspectOverride);
        if (cameraDebug)
            fprintf(stderr, "[NativeSettings] Camera debug overlay: ON\n");
        if (cameraBlend)
            fprintf(stderr, "[NativeSettings] Camera blend: ON\n");
        if (camForwardOffset != 0 || camHeightOffset != 0 || camLateralOffset != 0)
            fprintf(stderr, "[NativeSettings] Camera offset: forward=%.1f height=%.1f lateral=%.1f\n",
                    camForwardOffset, camHeightOffset, camLateralOffset);
    }

    static NativeSettings &Get() {
        static NativeSettings instance;
        return instance;
    }
};
