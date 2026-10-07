// SpotBeamHook — engine-side storage for the spotlight beam seam. See
// SpotBeamHook.h.

#include "platform/SpotBeamHook.h"

namespace {
NativeSpotBeamRenderer* gSpotBeamRenderer = nullptr;
}

void SetNativeSpotBeamRenderer(NativeSpotBeamRenderer* r) { gSpotBeamRenderer = r; }
NativeSpotBeamRenderer* GetNativeSpotBeamRenderer() { return gSpotBeamRenderer; }
