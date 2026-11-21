// ContactShadowHook — the seam between a consumer's crowd and the GPU
// backend's soft contact shadows (DC3 enhanced look, np-look32).
//
// DC3's crowd has no shadow of any kind: WorldCrowd::Draw3DChars switches self
// shadow and floor shadow off in game and the crowd models are authored
// without them, so on pale ground the crowd floats. The enhanced look gives
// each 3D crowd member a soft dark ellipse on the ground at its feet
// (LookSettings::crowdContactShadow). The original draws none; the retail look
// and the consumers with no look switch (rb3, rb3-xenon) draw none either.
//
// The consumer hands the backend one NativeContactShadow per crowd member,
// while the camera it is drawn with is current. The backend decides whether
// it draws at all (the look, the pass: only the main frame pass, never a
// render target, the shadow map or the HUD overlay), keeps the shadows, and
// draws them after the opaque world at that camera (before the deferred
// transparent draws), multiply-blended, depth tested against the world and
// the characters, writing no depth and no alpha.
//
// This header includes nothing and names no WebGPU type.

#ifndef MILO_ENGINE_PLATFORM_CONTACTSHADOWHOOK_H
#define MILO_ENGINE_PLATFORM_CONTACTSHADOWHOOK_H

// One shadow: an ellipse in the ground plane, world space.
struct NativeContactShadow {
    float centre[3];  // under the member, on its ground plane (the consumer lifts it
                      // a little off the floor so the floor's depth does not hide it)
    float axisA[3];   // half extent along the member's side-to-side axis
    float axisB[3];   // half extent along its front-to-back axis
};

class NativeContactShadowRenderer {
public:
    virtual ~NativeContactShadowRenderer() {}
    // True when a shadow queued now would be drawn (the look asks, and the
    // open pass is the main frame's). A consumer may skip its own work on false.
    virtual bool WantsContactShadows() = 0;
    // Keeps one shadow for the open pass; false when it will not be drawn.
    virtual bool QueueContactShadow(const NativeContactShadow& s) = 0;
};

// The backend's renderer; null when the linked backend draws none.
void SetNativeContactShadowRenderer(NativeContactShadowRenderer* renderer);
NativeContactShadowRenderer* GetNativeContactShadowRenderer();

#endif // MILO_ENGINE_PLATFORM_CONTACTSHADOWHOOK_H
