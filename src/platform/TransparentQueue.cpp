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
// W0.3c.S1 — draw-submission-order diagnostic probe (RB3_DRAWORDER_TRACE)
// ----------------------------------------------------------------------------
// DEFAULT-OFF, presence-read. INERT when RB3_DRAWORDER_TRACE is unset: the gate
// is a cached-static branch, so the flush hot path is byte-identical (the probe
// only adds stderr lines when on — never touches the drawlog JSON or draw order).
// Prints, per transparent flush, the queue order BEFORE and AFTER the non-stable
// std::sort, plus the (unsorted) text-queue order — with each entry's mesh
// pointer, FNV-1a name hash (SAME hash the RB3_DRAWLOG JSON uses, so lines cross-
// reference the residual sidecar), distSq, and the pre-sort insertion-seq. S1
// bisects across >=15 boots to attribute the ~33% order flake between the sort
// (mechanism 1) and allocation/traversal input order (mechanism 2).
static bool DrawOrderTraceOn() {
    static int sEnabled = -1;
    if (sEnabled < 0) {
        const char* e = getenv("RB3_DRAWORDER_TRACE");
        sEnabled = (e != nullptr) ? 1 : 0;
    }
    return sEnabled != 0;
}

// FNV-1a of a NUL-terminated string (empty/NULL -> 0). Identical constants to
// RB3DrawLogFnv1a in Rnd_Wgpu_RB3.cpp so the hashes match the drawlog JSON.
static unsigned long long DrawOrderNameHash(const char* s) {
    if (!s || !s[0]) return 0ull;
    unsigned long long h = 1469598103934665603ull; // FNV offset basis
    for (; *s; ++s) {
        h ^= (unsigned long long)(unsigned char)*s;
        h *= 1099511628211ull;                       // FNV prime
    }
    return h;
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
    if (DrawOrderTraceOn()) {
        static int sTextFlush = 0;
        int flush = sTextFlush++;
        for (size_t i = 0; i < draws.size(); ++i) {
            RndMesh* m = draws[i].mesh;
            fprintf(stderr, "RB3_DORDER textflush=%d stage=text i=%zu ptr=%p hash=0x%llx\n",
                    flush, i, (void*)m, DrawOrderNameHash(m ? m->Name() : nullptr));
        }
    }
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

    // W0.3c.S1 probe: capture pre-sort (= insertion) order before the sort so we
    // can (a) print it and (b) map each post-sort entry back to its insertion-seq.
    const bool traceOn = DrawOrderTraceOn();
    std::vector<const void*> preOrder; // insertion-order mesh ptrs (trace only)
    int flushIdx = 0;
    if (traceOn) {
        static int sFlush = 0;
        flushIdx = sFlush++;
        preOrder.reserve(draws.size());
        for (size_t i = 0; i < draws.size(); ++i) {
            RndMesh* m = draws[i].mesh;
            preOrder.push_back((const void*)m);
            fprintf(stderr,
                "RB3_DORDER flush=%d stage=pre i=%zu ptr=%p hash=0x%llx distSq=%.9g\n",
                flushIdx, i, (void*)m, DrawOrderNameHash(m ? m->Name() : nullptr),
                (double)draws[i].distSq);
        }
    }

    // Sort back-to-front (farthest first)
    std::sort(draws.begin(), draws.end(),
        [](const DeferredDraw& a, const DeferredDraw& b) {
            return a.distSq > b.distSq;
        });

    if (traceOn) {
        std::vector<char> used(preOrder.size(), 0);
        for (size_t i = 0; i < draws.size(); ++i) {
            RndMesh* m = draws[i].mesh;
            // First-unused pre-sort slot with a matching ptr = this entry's seq.
            int seq = -1;
            for (size_t j = 0; j < preOrder.size(); ++j) {
                if (!used[j] && preOrder[j] == (const void*)m) { seq = (int)j; used[j] = 1; break; }
            }
            fprintf(stderr,
                "RB3_DORDER flush=%d stage=post i=%zu seq=%d ptr=%p hash=0x%llx distSq=%.9g\n",
                flushIdx, i, seq, (void*)m, DrawOrderNameHash(m ? m->Name() : nullptr),
                (double)draws[i].distSq);
        }
    }

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
