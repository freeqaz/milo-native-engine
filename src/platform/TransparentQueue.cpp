// DC3 Native Port — Transparent/Text Draw Queue
// Deferred draw infrastructure for back-to-front alpha-blended rendering.
// Extracted from Mesh_Wgpu.cpp — no behavior change.

#include "platform/TransparentQueue.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/Mesh.h"
#include "rndobj/BaseMaterial.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Forward declaration — defined in Mesh_Wgpu.cpp
extern void DrawMeshImmediate(RndMesh* mesh);

// ============================================================================
// Transparent draw queue
// ============================================================================

struct DeferredDraw {
    RndMesh* mesh;
    float distSq; // squared distance from camera to centroid
    RndCam* cam;  // camera active when queued (restored during flush)
    RndEnviron* env; // environment active when queued
};
static std::vector<DeferredDraw> sTransparentQueue;
static bool sFlushingTransparentQueue = false;

// ============================================================================
// Text draw queue
// ============================================================================

struct TextDraw {
    RndMesh* mesh;
    RndCam* cam;
    RndEnviron* env;
};
static std::vector<TextDraw> sTextQueue;

// ============================================================================
// Blend classification
// ============================================================================

bool NoTransparentDefer() {
    static bool checked = false;
    static bool disabled = false;
    if (!checked) {
        disabled = (getenv("MILO_NO_TRANSPARENT_DEFER") != nullptr);
        checked = true;
        if (disabled) printf("DC3 Native: transparent defer disabled\n");
    }
    return disabled;
}

bool IsTransparentBlend(int blend) {
    return blend == BaseMaterial::kBlendSrcAlpha ||
           blend == BaseMaterial::kBlendSrcAlphaAdd ||
           blend == BaseMaterial::kBlendAdd ||
           blend == BaseMaterial::kBlendSubtract ||
           blend == BaseMaterial::kPreMultAlpha;
}

// ============================================================================
// Queue operations
// ============================================================================

bool HasTransparentDraws() {
    return !sTransparentQueue.empty();
}

bool IsFlushingTransparentDraws() {
    return sFlushingTransparentQueue;
}

void QueueTransparentDraw(RndMesh* mesh, float distSq, RndCam* cam, RndEnviron* env) {
    sTransparentQueue.push_back({mesh, distSq, cam, env});
}

// ============================================================================
// Flush functions
// ============================================================================

// Flush text draws — called from EndDrawing before transparent flush
void FlushTextDraws() {
    if (sTextQueue.empty()) return;
    std::vector<TextDraw> draws;
    draws.swap(sTextQueue);
    RndCam* savedCam = RndCam::Current();
    RndEnviron* savedEnv = RndEnviron::Current();
    for (auto& td : draws) {
        if (td.env && td.env != RndEnviron::Current())
            td.env->Select(nullptr);
        if (td.cam && td.cam != RndCam::Current())
            td.cam->Select();
        DrawMeshImmediate(td.mesh);
    }
    if (savedCam && savedCam != RndCam::Current())
        savedCam->Select();
    if (savedEnv && savedEnv != RndEnviron::Current())
        savedEnv->Select(nullptr);
}

// Called from EndDrawing to flush transparent draws
void FlushTransparentDraws() {
    if (sTransparentQueue.empty() || sFlushingTransparentQueue) return;

    sFlushingTransparentQueue = true;
    std::vector<DeferredDraw> draws;
    draws.swap(sTransparentQueue);

    // Save current camera/env so we can restore after processing deferred draws.
    // Each deferred draw restores its own camera, but the caller expects the
    // camera to remain unchanged after the flush.
    RndCam* savedCam = RndCam::Current();
    RndEnviron* savedEnv = RndEnviron::Current();

    // Sort back-to-front (farthest first)
    std::sort(draws.begin(), draws.end(),
        [](const DeferredDraw& a, const DeferredDraw& b) {
            return a.distSq > b.distSq;
        });

    for (auto& dd : draws) {
        if (dd.env && dd.env != RndEnviron::Current())
            dd.env->Select(nullptr);
        // Restore the camera that was active when this mesh was queued
        if (dd.cam && dd.cam != RndCam::Current())
            dd.cam->Select();
        DrawMeshImmediate(dd.mesh);
    }

    // Restore the camera/env that was active before the flush so the
    // caller's camera state is not corrupted.
    if (savedCam && savedCam != RndCam::Current())
        savedCam->Select();
    if (savedEnv && savedEnv != RndEnviron::Current())
        savedEnv->Select(nullptr);

    sFlushingTransparentQueue = false;
}
