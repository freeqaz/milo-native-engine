// RB3 2D quad pipeline (DrawRect + the outfit two-colour compose lerp) —
// extracted from platform/Rnd_Wgpu_RB3.cpp (W1.4.S3). RB3-only TU: compiled
// into MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3, never alongside the DC3 draw
// path.
//
// This is NOT a convergence onto gfx/DrawRect2D (a DC3-flavor TU the RB3
// backend deliberately drops — see CMakeLists.txt around the GFX rndobj
// block): BandRnd::DrawRect modulates by mat->GetColor() * paramColor and
// carries the entire OutfitConfig two-colour compose path
// (gRB3OutfitComposeActive / mCompose* / the rb3_compose.wgsl lerp pass),
// whereas gfx/DrawRect2D::Draw ignores matColor and has no compose. New
// RB3-only TU only — see W1.4 PLAN.
//
// The quad methods are BandRnd:: members already declared in Rnd_Wgpu_RB3.h;
// this TU holds their DEFINITIONS (moved verbatim):
//   EnsureQuadPipeline / DrawRect, plus the file-scope kRB3QuadShaderSource
//   include, the RB3RectUB / RB3RectVertex CPU mirror structs, the static
//   RB3QuadPipeKey() helper (moves with its only callers, stays static), and
//   the non-static gRB3OutfitComposeActive global (OutfitConfig.cpp externs
//   it — relocated definition only, no header change).
#pragma once

// RB3_RTT_OFF=1 canary (Rnd_Wgpu_RB3.cpp:BeginDrawTarget/DrawMesh + here in
// DrawRect's lazy RTT begin-hook). De-static'd (W1.4.S3 linkage MOVE,
// analogue of W1.4.S2's RB3PostProcDisabled expose): DrawRect moves to this
// TU but the definition stays in Rnd_Wgpu_RB3.cpp (its other two call sites,
// BeginDrawTarget and DrawMesh, are out of W1.4 scope).
bool RB3RttDisabled();
