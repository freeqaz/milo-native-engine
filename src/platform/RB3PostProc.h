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

// Stage-2 grade uniform block (ported from gfx/PostProcPass.cpp). Declared here
// (not in RB3PostProc.cpp) because it is SHARED cross-TU: RunPostProcComposite
// fills it, and the staying BandRnd::EnsureQuadPipeline in Rnd_Wgpu_RB3.cpp (→
// RB3Quad in W1.4.S3) sizes the postproc UB to sizeof(PostProcUniforms). Moved
// verbatim from Rnd_Wgpu_RB3.cpp — layout/static_assert preserved (W1.4.S2).
struct PostProcUniforms {
    float contrast;
    float brightness;
    float saturation;
    float vignetteIntensity;
    float vignetteColor[4];
    float chromaticOffset;
    float chromaticSharpen;
    float posterLevels;
    float posterMin;
    float levelInLo[4];
    float levelInHi[4];
    float levelOutLo[4];
    float levelOutHi[4];
    float bloomIntensity;
    float noiseIntensity;
    float noiseMidtone;
    float flickerMul;
    float bloomColor[4];
    float time;
    // Repurposed tail pad (keeps the struct at 160B so the layout/static_assert
    // are preserved): tiled-noise-texture controls. noiseHasMap selects the
    // textured grain path (1.0) vs the procedural hash fallback (0.0).
    float noiseScaleX;   // was _pad0 — mNoiseBaseScale.x (X tiling)
    float noiseScaleY;   // was _pad1 — mNoiseBaseScale.y (Y tiling)
    float noiseHasMap;   // was _pad2 — 1.0 if a noise bitmap is bound
};
static_assert(sizeof(PostProcUniforms) == 160, "PostProcUniforms must be 160 bytes");

// The Stage-2 grade WGSL (fs_postproc / vs_fullscreen). De-static'd from
// Rnd_Wgpu_RB3.cpp: shared by RunPostProcComposite AND the staying
// EnsureQuadPipeline (both reference kRB3PostProcShaderSource). W1.4.S2 linkage
// MOVE — single non-static definition lives in RB3PostProc.cpp.
extern const char* kRB3PostProcShaderSource;
