// PointTestHook — the seam between a consumer's flare point test and the GPU
// backend's occlusion queries.
//
// Retail RB3 (Xbox 360) answers a flare's visibility with occlusion queries.
// Rnd::TestPoint queues {screen x, screen y, projected z, flare}; at world end
// DxRnd::DoPointTests (rb3-xenon rnddx9/Rnd_Xbox.cpp) first reads back the
// queries it issued the frame before:
//   point query answered -> flare->SetVisible(samples != 0)
//   area query answered  -> flare->SetOcclusionResult(samples)
// and then draws this frame's: a one-pixel point and the flare's area rect
// (flare->mArea, in screen pixels), both at the point's projected depth with
// depth test on, depth and colour writes off. RndFlare::DrawShowing divides the
// area result by the rect's area, so an unoccluded flare draws at full
// strength, a half-hidden one at half and a hidden one not at all.
//
// The consumer keeps the RndFlare side (its flare class differs per decomp) and
// talks to the backend through this header:
//   - NativePointTester (implemented by a GPU backend that has occlusion
//     queries; today the engine's dc3 WgpuRnd) queues a test and cancels the
//     tests of a flare that goes away;
//   - the consumer registers a NativePointTestResultFn, which the backend calls
//     with each answer, at world end, one frame after the test was queued.
// With no tester registered (headless builds, the rb3 BandRnd flavor) the
// consumer keeps its own fallback.
//
// This header includes nothing and names no Milo or WebGPU type.

#ifndef MILO_ENGINE_PLATFORM_POINTTESTHOOK_H
#define MILO_ENGINE_PLATFORM_POINTTESTHOOK_H

// One flare's test, as retail Rnd::TestPoint queues it.
struct NativePointTest {
    const void* key;    // the flare; opaque to the engine, handed back in the result
    float world[3];     // the tested point in world space (retail: the flare's
                        // position moved mOffset toward the camera)
    float screenX;      // the point on screen, 0..1 across (RndCam::WorldToScreen)
    float screenY;      // 0..1 down
    float rect[4];      // the area rect x, y, w, h in screen pixels (RndFlare::mArea)
    float screenW;      // the screen those pixels measure (Rnd::Width/Height)
    float screenH;
    bool pointTest;     // issue the one-pixel point query
    bool areaTest;      // issue the area query
};

// One answer, delivered at world end one frame after the test was queued.
struct NativePointTestResult {
    const void* key;
    bool pointDone;     // the point query answered ...
    bool visible;       // ... and at least one sample passed
    bool areaDone;      // the area query answered ...
    float area;         // ... with this many visible pixels, in the test's
                        // screenW x screenH pixels (samples / MSAA sample count)
};

class NativePointTester {
public:
    virtual ~NativePointTester() {}
    // Queue a test for this frame's world end. False when the backend cannot
    // test it now (no frame, no camera); the consumer then answers it itself.
    virtual bool QueuePointTest(const NativePointTest& test) = 0;
    // Forget every queued and in-flight test of `key`: no result for it will be
    // delivered after this returns.
    virtual void CancelPointTests(const void* key) = 0;
};

typedef void (*NativePointTestResultFn)(const NativePointTestResult& result);

// The backend's tester; null when the linked backend has no occlusion queries.
void SetNativePointTester(NativePointTester* tester);
NativePointTester* GetNativePointTester();

// The consumer's result handler; the backend drops answers while it is null.
void SetNativePointTestResultFn(NativePointTestResultFn fn);
NativePointTestResultFn GetNativePointTestResultFn();

#endif // MILO_ENGINE_PLATFORM_POINTTESTHOOK_H
