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
// (UiShadesInGammaSpace, UiUsesRetailMaterialTerms). frameInOverlay: the frame
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
// material.unlit under the retail UI terms: a lit material with use_environ 0
// takes an ambient of 1 and no lights (NgMat::SetupAmbient; the shader options
// set mRealLights/mApproxLights only for use_environ materials), so it shows
// its colour times its texture whatever environ is current.
bool MaterialShadesUnlit(bool matPrelit, bool matUseEnviron, bool retailMaterialTerms);

// Build material parameters for a multi-pass material (simplified, no heuristics)
MaterialParams BuildPassMaterialParams(BaseMaterial* pass);

// A text mesh's material routes atlas alpha into RGB (useAlphaAsRGB), right for
// an atlas that keeps glyphs in alpha with RGB == 0. True when the game's
// render hook says this mesh's font atlas carries glyph colour in RGB instead
// (DrawMaterialPolicy::isColorIcon), so the RGB must be sampled. False with no
// hook registered.
class RndMesh;
bool TextMeshKeepsAtlasRGB(RndMesh* mesh, RndMat* mat, bool skinned);
