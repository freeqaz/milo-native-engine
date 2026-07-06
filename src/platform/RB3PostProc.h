// RB3 Stage-2 post-process composite — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.4). RB3-only TU: compiled into
// MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3, never alongside the DC3 draw path.
//
// This is NOT a convergence onto gfx/PostProcPass (a DC3-flavor TU the RB3
// backend deliberately drops — see CMakeLists.txt around the GFX rndobj block):
// BandRnd::RunPostProcComposite runs its own bloom (mBloom) + 160-byte
// PostProcUniforms + mQuadPost* pipeline and grades an RB3-owned RTT
// intermediate, whereas PostProcPass::Run has a different signature/state
// (DofPass, flicker) and consumes DC3 RndPostProc. New RB3-only TU only.
//
// The composite methods are BandRnd:: members already declared in
// Rnd_Wgpu_RB3.h; this TU holds their DEFINITIONS (moved verbatim):
//   FlushPostProcMidFrame / DoPostProcess / MainColorTarget /
//   EnsureIntermediate / RunPostProcComposite, plus the PostProcUniforms struct,
//   the kRB3PostProcShaderSource WGSL include, and RB3PostProcDisabled().
//
// The one declaration this header carries is RB3PostProcDisabled(): the
// RB3_PP_OFF A/B canary gate. It is called from BOTH the staying
// BeginFrame/EndFrame/ClearDepthForOverlay in Rnd_Wgpu_RB3.cpp AND the moving
// postproc methods here, so it is de-static'd (its own linkage-MOVE commit,
// mirroring W1.3's GetRB3TexView expose) and declared once here. See W1.4 PLAN.
#pragma once

// RB3_PP_OFF=1 forces the whole postproc intermediate path inactive (frame
// renders straight to the framebuffer, no composite) — the Stage-2 A/B canary.
bool RB3PostProcDisabled();
