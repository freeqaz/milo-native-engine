#pragma once
#include <vector>
#include <cstdint>

class RndMesh;
class RndMat;
class RndTex;

enum Heuristic : uint32_t {
    // Correct engine behavior (tracked for diagnostics)
    kHeuristicEmissiveGuard      = 1 << 0, // Zeroed emissive multiplier (no emissive map present)
    kHeuristicFogBlendCheck      = 1 << 1, // Fog disabled for additive/subtractive blend
    kHeuristicTextMeshDetect     = 1 << 2, // Text mesh (no depth, no cull, prelit)
    kHeuristicTextAlphaAsRGB     = 1 << 3, // Font atlas alpha-as-RGB mode
    kHeuristicMultiplyPrelit     = 1 << 4, // Multiply blend forced prelit (light-catcher fix)
};

struct TexBindingInfo {
    const char* slotName;
    RndTex* source;
    bool uploaded;
    bool usingFallback;
};

struct DrawCallRecord {
    int index;
    const char* meshName;
    const char* materialName;
    const char* cameraName;

    // Pipeline state
    int blend, zMode, cull, stencil;
    bool skinned, alphaCut, alphaWrite;

    // Material uniforms snapshot
    float color[4];
    float specularPower;
    float emissiveMultiplier;
    float prelit;
    float useTexture;
    float alpha;

    // Spatial snapshot
    float worldPos[3];
    float ndcPos[3];
    bool hasNdcPos;

    // Heuristics
    uint32_t heuristicsApplied;
    static const char* HeuristicName(int bit);

    // Texture bindings
    TexBindingInfo texBindings[7];

    // Skip info
    bool skipped;
    const char* skipReason;

    // Transparent queue
    bool deferred;
    float distSq;
};

class FrameCapture {
public:
    static FrameCapture& Get();

    void BeginFrame(int frameID);
    void EndFrame();

    DrawCallRecord& AddDraw();
    DrawCallRecord& AddSkip(const char* meshName, const char* reason);

    bool IsCapturing() const { return mCapturing; }
    void CaptureNextFrame() { mCaptureNext = true; }
    void SetCaptureFrame(int frame) { mTargetFrame = frame; }

    int RecordCount() const { return (int)mRecords.size(); }

    void DumpToStderr() const;
    void DumpFiltered(const char* meshFilter, const char* matFilter, int blendFilter) const;

private:
    std::vector<DrawCallRecord> mRecords;
    int mFrameID = 0;
    bool mCapturing = false;
    bool mCaptureNext = false;
    int mTargetFrame = -1;
};
