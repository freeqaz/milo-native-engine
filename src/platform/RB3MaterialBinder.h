// RB3 material -> MaterialUniforms translation — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.3). RB3-only TU: compiled into
// MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3, never alongside the DC3 draw path.
//
// This is the RB3-only twin of DC3's gfx/MaterialSetup (MaterialSetup.cpp):
// they diverge heavily — RB3 carries ~490 lines of game-specific material
// heuristics (text/UI colour, crowd-dim, track-light surface/rails/gem-smasher/
// peakstate, tail_* fret colours, highlight_* UI bar, gem_force, *_skin_diffuse
// debug probes) that BuildMaterialParams has none of — so the two are NOT
// convergeable. See W1.3 PLAN §"Why a NEW RB3-only TU".
//
// W1.3.S1 lands this header (interface only). W1.3.S2 moves the DrawMesh
// material->uniform block into RB3BuildMaterialUniforms (RB3MaterialBinder.cpp).
#pragma once

// NB: include the specific gfx headers, NOT platform/Rnd_Wgpu.h — the latter is
// the DC3 draw-path header and pulls in rndobj/Rnd_NG.h, which does not exist in
// this engine tree (the RB3 backend uses Rnd_Wgpu_RB3.h, which likewise includes
// these two directly). Mirrors RB3MeshCache.h / Rnd_Wgpu_RB3.h include style.
#include "gfx/UniformStructs.h"   // MaterialUniforms
#include "gfx/GpuDevice.h"        // GpuDevice (passed by ref to the tex helper)

#include <webgpu/webgpu_cpp.h>

class RndMesh;
class RndMat;
class RndTex;

// Cross-TU decls for the two tex-resolve helpers that live in Rnd_Wgpu_RB3.cpp
// (de-static'd in W1.3.S1 so the binder can resolve/upload a material's diffuse
// view). GetRB3TexView returns the cached GPU view (empty if not yet uploaded);
// UploadRndTexIfNeeded uploads the bitmap on first access and returns its view.
wgpu::TextureView GetRB3TexView(RndTex* tex);
wgpu::TextureView UploadRndTexIfNeeded(GpuDevice& gpu, RndTex* tex);

// Outputs of the material->uniform translation that DrawMesh reads after the
// block: the filled uniforms plus the two heuristic flags consumed by the
// downstream PipelineKey blend/zmode derivation.
struct RB3MaterialBindResult {
    MaterialUniforms mu;
    bool isTextMeshHeur = false;  // consumed by DrawMesh pipeline-key (zMode)
    bool gemForce       = false;  // consumed by DrawMesh pipeline-key (blend/zMode)
};

// Verbatim relocation of the DrawMesh material->uniform block (W1.3.S2). Pure
// translation: no GPU submits other than the lazy diffuse upload the block
// already did (via UploadRndTexIfNeeded on `gpu`). Defined in
// RB3MaterialBinder.cpp; declared here in S1 (unreferenced decl links fine).
RB3MaterialBindResult RB3BuildMaterialUniforms(
    RndMesh* mesh, RndMat* mat, bool skinned, RndMesh* owner, GpuDevice& gpu);
