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

// RB3_PP_LUMA_CEILING=1 switches the composite's highlight-ceiling guard (the
// "Gameplay-entry first-frame flash" rolloff in rb3_postproc.wgsl.inc) from a
// PER-CHANNEL Reinhard rolloff to a LUMINANCE-preserving one: compress on luma
// and scale RGB uniformly so hue/saturation survive the compression, instead
// of the per-channel path desaturating a hot moment toward grey. Default-OFF
// (flag-first, W3.3-fix); identity below the ceiling knee either way, so
// flag-OFF is byte-identical to pre-flag behavior. See
// docs/native/engine-arch-review-2026-07-05/execution/W3.3/STATUS.md.
bool RB3PPLumaCeilingActive();

// WASH-fix (Wave 8 A.S2) FIX-H2 (RB3_PP_CHROMA_PRESERVE=1): venue-scoped chroma
// preservation for the composite grade. The B+W_film02 grade desaturates a hot
// venue moment (songMs ~2000-6000) to grey; when set, the venue-backdrop
// composite reconstructs its output from the ungraded scene chroma scaled to the
// graded luminance (uniform value-scaling preserves HSV saturation), keeping the
// RB3_PP_OFF hue/sat while retaining the grade's exposure. Default-OFF; only the
// venue-grade path (FlushPostProcMidFrame) is affected, so the menu/song_select
// B+W look and the flag-OFF path are byte-identical. See rb3_postproc.wgsl.inc.
bool RB3PPChromaPreserveActive();

// Wave-13 Lane G (RB3_UI_POST_GRADE=1): generalize the Tier-2 mid-frame venue
// flush to the MENU venue->UI boundary so menu UI (hub/song_select/partdiff)
// draws UNGRADED over the graded venue backdrop, instead of the whole frame
// (venue+UI) being graded once at EndFrame (which washes the focused-item text:
// hub p60/p5 1.95 default vs 2.20 with the grade off). Default-OFF; when unset
// the menu-flush-pending latch is never set, so FlushPostProcMidFrame keeps
// venueGrade=true (gameplay Tier-2) and the flag-OFF path is byte-identical. The
// game-side TRIGGER (PanelDir::DrawShowing venue->UI boundary) is wired
// separately (coordinator sign-off) — this accessor + the latch are the
// renderer-side machinery. See execution/UIGRADE/PLAN.md.
bool RB3UIPostGradeActive();

// Menu-flush-pending latch: the game-side menu venue->UI boundary trigger sets
// this immediately before invoking Rnd::EndWorld() (-> DoPostProcess ->
// FlushPostProcMidFrame), so the flush composites the venue with
// venueGrade=FALSE (Tier-1 menu semantic: chroma-preserve stays OFF, the
// authored B+W menu look is untouched — the A5 trap). FlushPostProcMidFrame
// consumes (reads-and-clears) it, defaulting to venueGrade=true (gameplay) when
// unset. File-scope in RB3PostProc.cpp; free functions so the game-side trigger
// TU can set it without touching the renderer's private state.
void RB3SetMenuUIFlushPending();
bool RB3ConsumeMenuUIFlushPending();

// Wave-14 U-CLEAN: flush-ONLY menu-UI post-grade seam. The game-side venue->UI
// boundary trigger (PanelDir::DrawShowing) calls this INSTEAD of the former
// TheRnd->ClearDepthForOverlay() drive. It sets the menu-flush latch (so the
// composite grades with venueGrade=false, the A5-safe Tier-1 menu semantic) and
// drives BandRnd::FlushPostProcMidFrame() DIRECTLY — the mid-frame venue grade
// with NO depth-clear side effect. ClearDepthForOverlay's else-branch cleared
// depth+stencil per subsequent menu UI dir (the note-highway fallback), which on
// song_select produced a visible red band on the SETLISTS row; driving the flush
// directly removes it while keeping the hub grade-exemption win. Gated on
// RB3_UI_POST_GRADE (default-OFF): a no-op when the flag is unset or rnd is null.
// The flush is idempotent per frame (mPostProcFlushed) and early-returns when no
// graded venue is pending, so extra menu dirs are safe. `rnd` is TheRnd, always a
// BandRnd on the native backend. Declared with a forward Rnd (defined in Rnd.h).
class Rnd;
void RB3FlushMenuUIPostGrade(Rnd* rnd);

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
    // W3.3-fix (RB3_PP_LUMA_CEILING): highlight-ceiling mode toggle, 0.0=off
    // (per-channel, today's behavior) / 1.0=on (luminance-preserving). Grown
    // by a full vec4 (not just one float) to keep the struct's size a
    // multiple of 16 bytes, which the WGSL uniform address space requires
    // given the vec4 members above; the 3 trailing floats are unused padding.
    float lumaCeilingActive;
    // WASH-fix (Wave 8 A.S2) FIX-H2: repurposed pads (struct stays 176B).
    // chromaPreserveActive=1.0 -> restore input chroma over graded luma;
    // venueGrade=1.0 -> this composite is grading the venue backdrop (scopes the
    // chroma-preserve fix away from the menu B+W look). See rb3_postproc.wgsl.inc.
    float chromaPreserveActive;
    float venueGrade;
    float _padLumaCeiling2;
};
static_assert(sizeof(PostProcUniforms) == 176, "PostProcUniforms must be 176 bytes");

// The Stage-2 grade WGSL (fs_postproc / vs_fullscreen). De-static'd from
// Rnd_Wgpu_RB3.cpp: shared by RunPostProcComposite AND the staying
// EnsureQuadPipeline (both reference kRB3PostProcShaderSource). W1.4.S2 linkage
// MOVE — single non-static definition lives in RB3PostProc.cpp.
extern const char* kRB3PostProcShaderSource;
