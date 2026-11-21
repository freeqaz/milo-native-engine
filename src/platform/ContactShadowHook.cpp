// ContactShadowHook — engine-side storage for the contact shadow seam. See
// ContactShadowHook.h.

#include "platform/ContactShadowHook.h"

namespace {
NativeContactShadowRenderer* gContactShadowRenderer = nullptr;
}

void SetNativeContactShadowRenderer(NativeContactShadowRenderer* r) { gContactShadowRenderer = r; }
NativeContactShadowRenderer* GetNativeContactShadowRenderer() { return gContactShadowRenderer; }
