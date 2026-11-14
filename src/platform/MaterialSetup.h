// DC3 Native Port — Material Setup
// Extracts material uniform filling and texture resolution from Mesh_Wgpu.cpp
// into reusable functions for primary and multi-pass draw calls.

#pragma once

#include "platform/Rnd_Wgpu.h"
#include <cstdint>

// RndMat / BaseMaterial come from platform/rndshape/RndShape.h (via Rnd_Wgpu.h):
// on RB3-Wii-shaped rndobj, BaseMaterial is an alias of RndMat.

// All material-related data needed for a draw call
struct MaterialParams {
    MaterialUniforms uniforms;
    WgpuRnd::MaterialTexViews texViews;
    SamplerDesc samplerDesc;
    SamplerDesc mapSamplerDesc;
    uint32_t heuristics;
};

// Build material parameters for a primary material (full heuristics)
MaterialParams BuildMaterialParams(RndMat* mat, bool isTextMesh);

// The overlay-phase material rules a game opts into through GameRenderHook
// (UiUsesRetailMaterialTerms). frameInOverlay: the frame
// has left its world (WgpuRnd::FrameInOverlay). Outside the overlay phase, or
// with no hook, gammaShading is the shape's model and retailMaterialTerms false.
struct UiDrawTerms {
    bool gammaShading = false;        // material.gammaShading
    bool retailMaterialTerms = false; // keep the mat's lighting; intensify scales alpha
};
UiDrawTerms UiDrawTermsFor(bool frameInOverlay);
// material.prelit: the material's own flag, or forced for simple render,
// multiply blend, and -- unless the retail UI terms apply -- text and
// overlay-pass meshes.
bool MaterialShadesPrelit(bool matPrelit, bool isTextMesh, bool isOverlayPass,
                          bool isMultiplyBlend, bool simpleRender,
                          bool retailMaterialTerms);
// Factor on the material alpha: 2 for an intensify material under the retail
// UI terms (DC3's standard.ps: `mul r0, r0, c5.yyyy`, alpha included), else 1.
float MaterialAlphaScale(bool intensify, bool retailMaterialTerms);
// The material colour as the shader sees it. A premultiplied-alpha material
// (blend 7, pipeline One / OneMinusSrcAlpha) has its rgb scaled by its alpha
// before the constant is set -- NgMat::SetRegularShaderConst (Mat_NG.cpp:191,
// 100% matched) and RB3 Wii's NgMat equivalent both call PreMultiplyAlpha --
// so a fading premult label dims with its alpha instead of adding its full
// colour until the alpha nears 0. Every other blend: the colour unchanged.
void MaterialShaderColor(const Hmx::Color& matColor, BaseMaterial::Blend blend, float out[4]);

// Build material parameters for a multi-pass material (simplified, no heuristics)
MaterialParams BuildPassMaterialParams(BaseMaterial* pass);

// The same two builders with the overlay-phase terms given rather than read
// from the frame (UiDrawTermsFor(FrameInOverlay())): the seam a test drives
// to check the terms reach the uniforms.
MaterialParams BuildMaterialParams(RndMat* mat, bool isTextMesh, const UiDrawTerms& uiTerms);
MaterialParams BuildPassMaterialParams(BaseMaterial* pass, const UiDrawTerms& uiTerms);

// A text mesh's material routes atlas alpha into RGB (useAlphaAsRGB), right for
// an atlas that keeps glyphs in alpha with RGB == 0. True when the game's
// render hook says this mesh's font atlas carries glyph colour in RGB instead
// (DrawMaterialPolicy::isColorIcon), so the RGB must be sampled. False with no
// hook registered.
class RndMesh;
bool TextMeshKeepsAtlasRGB(RndMesh* mesh, RndMat* mat, bool skinned);

// True when a mesh is drawn as a text glyph page: no depth test and no
// culling, whatever its material asks. When the render hook says the game marks
// its pages (GameRenderHook::MarksTextGlyphMeshes), only a mesh marked with
// MarkTextGlyphMesh() is one, so spotlight beams, ribbons, lines and crowd
// billboards -- unnamed too -- keep their material's zMode and cull. Without
// that the page is inferred: unnamed and not skinned (DC3's HamRibbon hand
// trail is unnamed but skinned).
bool MeshDrawsAsGlyph(RndMesh* mesh, bool skinned);

// True when a mesh takes the text colour path (BuildMaterialParams' isTextMesh:
// forced prelit, atlas alpha into RGB unless TextMeshKeepsAtlasRGB). A game
// that marks its glyph pages (MarksTextGlyphMeshes) gets it for marked meshes
// only, so an unnamed ribbon, beam or billboard shades with its own material
// (DC3's hamribbon.mat: lit, use_environ 0). Without the mark: every unnamed
// mesh, skinned or not, as before.
bool MeshShadesAsText(RndMesh* mesh);
