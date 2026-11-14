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

// Build material parameters for a multi-pass material (simplified, no heuristics)
MaterialParams BuildPassMaterialParams(BaseMaterial* pass);

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
