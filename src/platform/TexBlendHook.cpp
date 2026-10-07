// TexBlendHook — engine-side storage for the texture-blend seam. See
// TexBlendHook.h.

#include "platform/TexBlendHook.h"

namespace {
NativeTexBlendComposer* gTexBlendComposer = nullptr;
}

void SetNativeTexBlendComposer(NativeTexBlendComposer* composer) {
    gTexBlendComposer = composer;
}
NativeTexBlendComposer* GetNativeTexBlendComposer() { return gTexBlendComposer; }
