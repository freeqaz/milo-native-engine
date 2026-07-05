// RB3 additive-halo-only highway gem-bloom pass — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.4). RB3-only TU: compiled into
// MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3, never alongside the DC3 draw path.
//
// This pass has NO DC3 counterpart at all — it is the capture-and-replay gem
// bloom (Design B) unique to the RB3 backend: during the live game.cam highway
// pass, DrawMesh CAPTURES a per-draw replay record (pipeline + the LIVE pose-
// baked mSceneBindGroup HANDLE + mat/obj/bone bind groups + vbuf/ibuf), and at
// EndFrame CompositeHaloBloom replays those draws into a transparent buffer,
// blooms it, and ADDITIVE-blits ONLY the blurred halo onto mFrameView. The base
// highway is never redirected or re-composited. Because the capture stores the
// LIVE mSceneBindGroup handle (not a uint32_t offset), a dynamicOffsetCount
// mismatch on replay would silently discard the whole command buffer — so the
// W1.4 extraction is a pure token-for-token MOVE.
//
// The five methods below are BandRnd:: members already declared in
// Rnd_Wgpu_RB3.h; this TU only holds their DEFINITIONS (moved verbatim):
//   HighwayBloomEnabled / IsHaloSourceMat / EnsureHaloTarget /
//   EnsureHaloBlitPipeline / CompositeHaloBloom.
// It therefore declares nothing itself — the header exists for the CMake/comment
// convention (mirrors RB3MeshCache.h / RB3MaterialBinder.h). See W1.4 PLAN for
// why this is a NEW RB3-only TU, not a convergence onto gfx/PostProcPass.
#pragma once
