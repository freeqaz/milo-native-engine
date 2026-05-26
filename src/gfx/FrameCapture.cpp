#include "gfx/FrameCapture.h"
#include <cstdio>
#include <cstring>

static const char* sHeuristicNames[] = {
    "EmissiveGuard",
    "FogBlendCheck",
    "TextMeshDetect",
    "TextAlphaAsRGB",
};

const char* DrawCallRecord::HeuristicName(int bit) {
    if (bit >= 0 && bit < (int)(sizeof(sHeuristicNames)/sizeof(sHeuristicNames[0])))
        return sHeuristicNames[bit];
    return "Unknown";
}

FrameCapture& FrameCapture::Get() {
    static FrameCapture sInstance;
    return sInstance;
}

void FrameCapture::BeginFrame(int frameID) {
    mFrameID = frameID;
    if (mCaptureNext) {
        mCapturing = true;
        mCaptureNext = false;
    } else if (mTargetFrame >= 0 && frameID == mTargetFrame) {
        mCapturing = true;
    } else {
        mCapturing = false;
    }
    if (mCapturing) {
        mRecords.clear();
        mRecords.reserve(256);
    }
}

void FrameCapture::EndFrame() {
    if (mCapturing) {
        DumpToStderr();
        mCapturing = false;
        if (mTargetFrame == mFrameID)
            mTargetFrame = -1;
    }
}

DrawCallRecord& FrameCapture::AddDraw() {
    mRecords.emplace_back();
    auto& rec = mRecords.back();
    memset(&rec, 0, sizeof(rec));
    rec.index = (int)mRecords.size() - 1;
    rec.skipped = false;
    return rec;
}

DrawCallRecord& FrameCapture::AddSkip(const char* meshName, const char* reason) {
    mRecords.emplace_back();
    auto& rec = mRecords.back();
    memset(&rec, 0, sizeof(rec));
    rec.index = (int)mRecords.size() - 1;
    rec.meshName = meshName;
    rec.skipped = true;
    rec.skipReason = reason;
    return rec;
}

void FrameCapture::DumpToStderr() const {
    int draws = 0, skips = 0, defers = 0;
    for (auto& r : mRecords) {
        if (r.skipped) skips++;
        else if (r.deferred) defers++;
        else draws++;
    }
    fprintf(stderr, "\n=== FRAME CAPTURE #%d ===\n", mFrameID);
    fprintf(stderr, "Total records: %d (draws=%d, skips=%d, deferred=%d)\n\n",
            (int)mRecords.size(), draws, skips, defers);

    for (auto& r : mRecords) {
        if (r.skipped) {
            fprintf(stderr, "[%3d] SKIP  mesh='%s' reason='%s'\n",
                    r.index, r.meshName ? r.meshName : "?", r.skipReason ? r.skipReason : "?");
            continue;
        }
        fprintf(stderr, "[%3d] DRAW  mesh='%s' mat='%s' cam='%s' blend=%d zMode=%d cull=%d",
                r.index, r.meshName ? r.meshName : "?",
                r.materialName ? r.materialName : "?",
                r.cameraName ? r.cameraName : "?",
                r.blend, r.zMode, r.cull);
        if (r.skinned) fprintf(stderr, " SKINNED");
        if (r.deferred) fprintf(stderr, " DEFERRED(dist=%.1f)", r.distSq);
        fprintf(stderr, "\n");

        fprintf(stderr, "       color=(%.2f,%.2f,%.2f,%.2f) alpha=%.2f prelit=%.0f tex=%.0f spec=%.0f emissive=%.2f\n",
                r.color[0], r.color[1], r.color[2], r.color[3],
                r.alpha, r.prelit, r.useTexture, r.specularPower, r.emissiveMultiplier);
        fprintf(stderr, "       world=(%.2f,%.2f,%.2f)",
                r.worldPos[0], r.worldPos[1], r.worldPos[2]);
        if (r.hasNdcPos) {
            fprintf(stderr, " ndc=(%.3f,%.3f,%.3f)",
                    r.ndcPos[0], r.ndcPos[1], r.ndcPos[2]);
        }
        fprintf(stderr, "\n");

        if (r.heuristicsApplied) {
            fprintf(stderr, "       heuristics: ");
            for (int b = 0; b < (int)(sizeof(sHeuristicNames)/sizeof(sHeuristicNames[0])); b++) {
                if (r.heuristicsApplied & (1u << b))
                    fprintf(stderr, "%s ", DrawCallRecord::HeuristicName(b));
            }
            fprintf(stderr, "\n");
        }

        for (int t = 0; t < 7; t++) {
            auto& tb = r.texBindings[t];
            if (tb.slotName) {
                fprintf(stderr, "       tex[%s]: %s%s\n",
                        tb.slotName,
                        tb.uploaded ? "uploaded" : "missing",
                        tb.usingFallback ? " (fallback)" : "");
            }
        }
    }
    fprintf(stderr, "=== END CAPTURE ===\n\n");
}

void FrameCapture::DumpFiltered(const char* meshFilter, const char* matFilter, int blendFilter) const {
    fprintf(stderr, "\n=== FRAME CAPTURE #%d (filtered) ===\n", mFrameID);
    for (auto& r : mRecords) {
        if (r.skipped) continue;
        if (meshFilter && r.meshName && !strstr(r.meshName, meshFilter)) continue;
        if (matFilter && r.materialName && !strstr(r.materialName, matFilter)) continue;
        if (blendFilter >= 0 && r.blend != blendFilter) continue;

        fprintf(stderr, "[%3d] mesh='%s' mat='%s' cam='%s' blend=%d color=(%.2f,%.2f,%.2f,%.2f)",
                r.index, r.meshName ? r.meshName : "?",
                r.materialName ? r.materialName : "?",
                r.cameraName ? r.cameraName : "?",
                r.blend, r.color[0], r.color[1], r.color[2], r.color[3]);
        fprintf(stderr, " world=(%.2f,%.2f,%.2f)",
                r.worldPos[0], r.worldPos[1], r.worldPos[2]);
        if (r.hasNdcPos) {
            fprintf(stderr, " ndc=(%.3f,%.3f,%.3f)",
                    r.ndcPos[0], r.ndcPos[1], r.ndcPos[2]);
        }
        if (r.heuristicsApplied) {
            fprintf(stderr, " heuristics=0x%x", r.heuristicsApplied);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== END FILTERED ===\n\n");
}
