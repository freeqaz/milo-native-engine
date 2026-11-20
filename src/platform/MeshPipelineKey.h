#pragma once

// The material pipeline key a mesh draw asks for, as a function of the mesh,
// its material and the pass it is drawn in (platform/Mesh_Wgpu.cpp). The draw
// (DrawMeshImmediate) and the load-time warm (WarmMeshPipelines; DC3 walks a
// dir it has just loaded, dc3 native/src/platform/PipelineWarm.cpp) both ask
// it, so a warm can only warm a key some draw would ask for -- the warm list
// is derived from the content, never recorded.

#include "gfx/PipelineManager.h"
// RndMesh, RndMat and BaseMaterial as the consumer's shape has them: a class
// on DC3-shaped rndobj, an alias of RndMat on RB3-Wii (where a forward
// `class BaseMaterial;` conflicts with the alias and rb3 does not compile).
#include "platform/rndshape/RndShape.h"

#include <cstdint>

// The pass state a mesh's key depends on.
struct MeshPassContext {
    wgpu::TextureFormat targetFormat = wgpu::TextureFormat::Undefined;
    uint32_t sampleCount = 1;
    bool hasDepth = true;     // false: the HUD overlay pass
    bool toTexture = false;   // a TexturePass (WgpuRnd::ActiveTargetTex)
    bool reflection = false;  // the reflection draw mode (cull flipped)
    // The self-shadow casters (rndshape::kSelfShadowCasterDrawMode, DC3's
    // kDrawExtrude): the image's shadowmap shader culls counter-clockwise
    // whatever the material asks (RndShader::CheckForceCull's mode 6, which a
    // kCullBackwards material also gets), so the map holds the far faces.
    bool selfShadowCaster = false;
};

// The pass being drawn now.
MeshPassContext CurrentMeshPassContext();

PipelineKey MeshPipelineKey(RndMesh* mesh, RndMat* rawMat, bool skinned, int32_t depthBias,
                            const MeshPassContext& ctx);
// The same for a material alone: what MeshPipelineKey computes once it has
// read the mesh (whether it is a glyph page, whether it shades as text).
PipelineKey MaterialPipelineKey(RndMat* rawMat, bool skinned, bool isGlyphMesh, bool isTextMesh,
                                int32_t depthBias, const MeshPassContext& ctx);
PipelineKey MeshNextPassPipelineKey(const PipelineKey& first, BaseMaterial* nextPass,
                                    const MeshPassContext& ctx);

// True when a draw with this key and this material alpha cannot change a
// pixel, a depth value or a stencil value, so it need not be sent:
//   - the material alpha is exactly 0. The fragment's alpha is that times the
//     texel's and the vertex's (standard_wgsl.inc shadeStandard), so it is 0
//     over the whole draw;
//   - the blend weights the source by its alpha and keeps the destination:
//     SrcAlpha (src*a + dst*(1-a)) and SrcAlphaAdd (src*a + dst). The alpha
//     channel follows the same factors, or max(src.a, dst.a) under retail's
//     destination alpha, and both leave dst.a. Add, PreMultAlpha, Multiply
//     and the rest draw at alpha 0 and are never skipped;
//   - the pass has no depth, or the z mode does not write it (Disable,
//     Transparent); the stencil mode does not write;
//   - no bloom mask: with one the shader's output alpha is the mask, not the
//     material's (bloomMaskScale != 0).
// A menu keeps its hidden layers this way: DC3's song select sent 161 such
// draws of 433 every frame, each rasterised and blended for nothing.
inline bool DrawChangesNothing(const PipelineKey& key, float materialAlpha, float bloomMaskScale) {
    if (materialAlpha != 0.0f || bloomMaskScale != 0.0f) return false;
    if (key.blend != WgpuBlend::SrcAlpha && key.blend != WgpuBlend::SrcAlphaAdd) return false;
    if (key.hasDepth && key.zMode != WgpuZMode::Disable && key.zMode != WgpuZMode::Transparent)
        return false;
    return key.stencil != WgpuStencil::Write;
}

// Material state a property animation can key (RndPropAnim on a material's
// blend, z_mode or alpha_write); -1 = the material's own.
struct MatKeyVariant {
    int blend = -1;
    int zMode = -1;
    int alphaWrite = -1;
};

// Queue (PipelineManager::WarmAsync) the keys `mesh` would ask for in `ctx`:
// its material's and every NextPass's, as the material is and under each of
// `variants`, and -- in the main frame, for a material that allows the bloom
// mask -- with the mask's alpha write too. A mesh the consumer filters
// (ShouldSkipMesh) is skipped, as its draw is. Returns the keys queued.
int WarmMeshPipelines(RndMesh* mesh, const MeshPassContext& ctx,
                      const MatKeyVariant* variants = nullptr, int numVariants = 0);

// The same for a mesh that is not in any dir until its first draw, made by an
// object that is (a ribbon builds its own mesh, skinned or not, from its
// material): the keys a plain mesh of `skinned` drawn with `rawMat` asks for.
int WarmMaterialPipelines(RndMat* rawMat, bool skinned, const MeshPassContext& ctx,
                          const MatKeyVariant* variants = nullptr, int numVariants = 0);

// Queue the keys a text page drawn with font material `fontMat` asks for in
// `ctx`. A text builds its pages when its string is set, so a page that only
// a later string needs (an icon font's, say) is in no loaded dir; its font
// is. Returns the keys queued.
int WarmTextPagePipelines(RndMat* fontMat, const MeshPassContext& ctx);
