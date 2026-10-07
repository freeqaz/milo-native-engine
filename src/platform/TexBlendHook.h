// TexBlendHook — the seam between a consumer's RndTexBlender and the GPU
// backend that composes its output texture.
//
// Retail RB3 (Xbox 360) RndTexBlender::DrawShowing (rb3-xenon
// rndobj/TexBlender.cpp) renders into its output texture, a renderable
// no-z RndTex:
//   1. the base map, as a rect over the whole target, blend off;
//   2. each active controller's mesh "unwrapped": drawn with the unwrapuv
//      shader, which places every vertex at its UV, so the faces cover the
//      part of the texture they map, and the pixel shader writes
//      tex(uv).rgb with the material alpha (retail ucode
//      `tfetch2D r0.xyz1, r0.xy, tf0; mul oC0, r0, r2`), blend SrcAlpha /
//      InvSrcAlpha. The texture is the blender's near map, far map, or the
//      controller's own override map, and the alpha is the controller's
//      blend amount.
// Band heads use it for the wrinkle normal map (head_wrinkle_output.tex).
//
// The consumer keeps the RndTexBlender side (its classes differ per decomp):
// it decides which controllers draw, in which order and with which texture and
// alpha, exactly as retail does, and hands the list to the backend through
// this header. With no composer registered (headless builds, the rb3 BandRnd
// flavor) the consumer keeps its fallback.
//
// This header names Milo types only as forward declarations.

#ifndef MILO_ENGINE_PLATFORM_TEXBLENDHOOK_H
#define MILO_ENGINE_PLATFORM_TEXBLENDHOOK_H

class RndTex;
class RndMesh;

// One unwrapped draw: `mesh`'s faces (through its geometry owner) at their
// UVs, textured by `tex`, written with alpha `alpha`.
struct NativeTexBlendLayer {
    RndMesh* mesh;
    RndTex* tex;
    float alpha;
};

class NativeTexBlendComposer {
public:
    virtual ~NativeTexBlendComposer() {}
    // Compose `output` now: the `base` rect (skipped when null) and then the
    // `count` layers, in order. False when nothing was drawn because the
    // backend cannot draw now (no frame being recorded, `output` not
    // renderable); the output keeps its previous contents.
    virtual bool ComposeTexBlend(RndTex* output, RndTex* base,
                                 const NativeTexBlendLayer* layers, int count) = 0;
};

// The backend's composer; null when the linked backend has none.
void SetNativeTexBlendComposer(NativeTexBlendComposer* composer);
NativeTexBlendComposer* GetNativeTexBlendComposer();

#endif // MILO_ENGINE_PLATFORM_TEXBLENDHOOK_H
