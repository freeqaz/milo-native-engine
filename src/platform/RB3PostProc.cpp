// RB3 Stage-2 post-process composite — extracted VERBATIM from BandRnd:: methods
// in platform/Rnd_Wgpu_RB3.cpp (W1.4.S2). RB3-only TU
// (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3). Pure MOVE: the mid-frame venue grade,
// the 160-byte PostProcUniforms block, the fullscreen composite, and the
// RB3_PP_OFF canary are copied token-for-token. See RB3PostProc.h / W1.4 PLAN
// for why this is a NEW RB3-only TU, not a convergence onto gfx/PostProcPass.
//
// Moved defs (all BandRnd:: members already declared in Rnd_Wgpu_RB3.h, plus the
// file-scope PostProcUniforms struct + kRB3PostProcShaderSource include + the
// now-cross-TU RB3PostProcDisabled): FlushPostProcMidFrame, DoPostProcess,
// MainColorTarget, EnsureIntermediate, RB3PostProcDisabled, RunPostProcComposite.
// RunPostProcComposite calls EnsureQuadPipeline() (lands in RB3Quad, S3) as a
// link-time symbol; ClearDepthForOverlay / EnsureDepth / the prewarm helpers
// stay in Rnd_Wgpu_RB3.cpp (out of W1.4 scope).

#include "platform/Rnd_Wgpu_RB3.h"
#include "platform/RB3PostProc.h"
#include "platform/RB3MaterialBinder.h"  // W1.3: UploadRndTexIfNeeded (noise-map upload)

#include "rndobj/Cam.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"

#include <cstdio>
#include <cstdlib>
#include <string>  // BOOTRNG A.S1: throttle key for the PPRESOLVED probe
#include <cstring>

// ===========================================================================
// Tier 2 mid-frame layering — grade the VENUE, draw highway+HUD over it ungraded
// (this supersedes the concurrent depth-only ClearDepthForOverlay from engine
//  6498fab: the Tier 2 ClearDepthForOverlay below does the venue-grade flush AND
//  the original depth-clear as its fallback — see its body.)
//
// Retail layers the post-process as a fullscreen TEV blit of the world RTT inside
// EndWorld (WiiRnd::DoPostProcess), AFTER the venue scene and BEFORE the HUD/track
// panel. We reproduce that: when EndWorld fires (PanelDir::DrawShowing, once per
// frame, via the engine's mWorldEnded latch), the fully-rendered venue
// intermediate is graded onto the framebuffer here, then the main pass RESUMES
// targeting the framebuffer — color LoadOp::Load keeps the graded venue, depth
// LoadOp::Clear lets the highway/gems/HUD composite ON TOP, UNGRADED. Reuses the
// EndDrawTarget suspend/resume machinery (no new GPU plumbing).
// ===========================================================================
void BandRnd::FlushPostProcMidFrame() {
    // Wave-13 Lane G: consume the menu-flush latch UNCONDITIONALLY, before the
    // early-returns. The game-side menu trigger (PanelDir::DrawShowing) sets it
    // then calls ClearDepthForOverlay -> here; if this flush no-ops (no
    // active postproc, venue not yet in the intermediate, already flushed) the
    // latch must NOT dangle into a later frame / the next gameplay flush (which
    // would wrongly composite with venueGrade=false). Consuming here bounds it to
    // exactly this call. Gameplay never sets the latch (setter is only called from
    // the menu venue->UI boundary), so this reads false and venueGrade stays true
    // below — byte-identical to the old hardcode.
    const bool menuBoundary = RB3ConsumeMenuUIFlushPending();
    if (!mGpuReady || mPostProcFlushed || !mRenderedToIntermediate) return;
    if (RB3PostProcDisabled() || !RndPostProc::Current() || !mIntermediateView) return;
    if (!mFrameView) return;                 // frame already torn down (defensive)
    if (mRtActiveTex) return;                // never flush while a mid-frame RTT pass is open

    // 1. Close the main (intermediate) pass so the venue is fully written.
    if (mInPass) { mPass.End(); mInPass = false; }

    // 2. Grade the intermediate onto the framebuffer (runs bloom + composite; opens
    //    and closes its own render pass against mFrameView).
    //    Wave-13 Lane G: when this flush was triggered at a MENU venue->UI
    //    boundary (RB3_UI_POST_GRADE, latch set by the game-side trigger), grade
    //    with venueGrade=FALSE — the Tier-1 menu semantic — so the default-ON
    //    chroma-preserve (gated on venueGrade>0.5) stays OFF and the authored B+W
    //    menu look is untouched (A5 trap). Unset (gameplay Tier-2) -> venueGrade
    //    stays true, byte-identical to before. (menuBoundary was consumed at the
    //    top of this function so a no-op flush can't leave the latch dangling.)
    RunPostProcComposite(mFrameView, /*venueGrade=*/!menuBoundary);
    mPostProcFlushed = true;

    // 3. Re-open the main pass targeting the FRAMEBUFFER. Color LoadOp::Load keeps
    //    the graded venue; depth LoadOp::Clear resets z so the highway/gems/HUD
    //    (drawn with their own game.cam) composite on top instead of being occluded
    //    by venue geometry depth. (Same suspend/resume contract as EndDrawTarget;
    //    depthClearValue must be finite for Dawn validation even with Load.)
    //
    //    Wave-14 U-CLEAN: at a MENU venue->UI boundary (menuBoundary — the
    //    RB3_UI_POST_GRADE grade-exemption flush), re-open with depth LoadOp::Load
    //    (PRESERVE the venue depth) instead of Clear. The menu win only needs the
    //    COLOR composite (graded venue, UI ungraded on top); additionally CLEARING
    //    depth reveals z-occluded menu UI — on song_select the SETLISTS-row
    //    selection quad (occluded in the flag-OFF layering, only its right edge
    //    poking past the album panel) drew full-width as a red band. Preserving
    //    depth keeps occluded UI occluded and removes the band. Gameplay
    //    (menuBoundary=false) keeps LoadOp::Clear so the highway/HUD composite over
    //    venue geometry — byte-identical to before (the latch is never set outside
    //    the menu trigger). Stencil follows depth for the same reason.
    wgpu::LoadOp menuDepthOp =
        menuBoundary ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    // W17 R3-UIDUMP G4 test knob (RB3_MENU_DEPTH_CLEAR): force the menu-boundary
    // re-open depth LoadOp back to Clear on demand, re-manifesting the Wave-14
    // SETLISTS red band (z-occluded UI revealed). Test-only, default-OFF; the
    // shipped state preserves LoadOp::Load at the boundary. Cached getenv.
    if (menuBoundary) {
        static int sMenuDepthClear = -1;
        if (sMenuDepthClear < 0) {
            const char* e = getenv("RB3_MENU_DEPTH_CLEAR");
            sMenuDepthClear = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (sMenuDepthClear) menuDepthOp = wgpu::LoadOp::Clear;
    }

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = mFrameView;
    colorAtt.loadOp = wgpu::LoadOp::Load;    // preserve the graded venue blit
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = menuDepthOp; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = menuDepthOp; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandMainPassPostGrade";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rp);
    mInPass = true;
    // W17 R3-UIDUMP: note this pass + its EFFECTIVE depth LoadOp (0 Clear, 1 Load)
    // for the provenance sidecar — so /api/uidump reports whether a menu-boundary
    // draw's pass cleared or preserved depth (the G4 red-band diagnosis datum).
    ProvNotePassOpen(menuDepthOp == wgpu::LoadOp::Clear ? 0 : 1);
    mPass.SetBindGroup(0, mActiveScene.group, 0, nullptr);
    mLastSceneCam = nullptr;   // next DrawMesh re-resolves the active cam

    if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_TIER2_DBG] mid-frame venue composite flushed f%d "
                "meshesDrawnSoFar=%d (highway/HUD draws ungraded over graded venue)\n",
                mFrameCount, mDrawnMeshes);
}

void BandRnd::DoPostProcess() {
    // Preserve the base post-processor bookkeeping (mPostProcessors is empty on
    // the native backend, but DoWorldEnd/DoPost state stays consistent), then run
    // the mid-frame venue grade composite. Fires once per frame via the
    // mWorldEnded latch in Rnd::EndWorld (the caller).
    Rnd::DoPostProcess();
    if (getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_TIER2_DBG] DoPostProcess f%d meshes=%d hasPP=%d flushed=%d toInt=%d\n",
                mFrameCount, mDrawnMeshes, RndPostProc::Current() != nullptr,
                mPostProcFlushed, mRenderedToIntermediate);
    FlushPostProcMidFrame();
}

// ===========================================================================
// Stage 2 postproc grade — PORTED VERBATIM from milo-native-engine
// src/gfx/PostProcPass.cpp:9-166 (uniform struct + WGSL). Lives in its own
// module (vs_fullscreen/fs_postproc) — see Rnd_Wgpu_RB3.h note on why it can't
// share kRB3QuadShaderSource (binding-2 type clash + bloomTex@3).
//
// B+W_film02.pp is a pure grade (saturation -40, contrast +10, black-lift) +
// bloom + vignette + noise — no refract/DOF/chromatic. v1 binds the black
// default to bloomTex@3 and skips the blur (the grade alone removes the smear);
// the screen-blend bloom term is then a no-op (bloom == black).
// ===========================================================================
// PostProcUniforms struct + static_assert relocated to RB3PostProc.h (shared
// type: RunPostProcComposite below AND the staying EnsureQuadPipeline in
// Rnd_Wgpu_RB3.cpp size their UBs to sizeof(PostProcUniforms)). W1.4.S2.
const char* kRB3PostProcShaderSource =
#include "gfx/Shaders/rb3_postproc.wgsl.inc"
;

// ===========================================================================
// Stage 2: postproc render-to-texture composite (§4 of the RTT plan).
//
// MainColorTarget(): the color attachment the main scene draws into. When a
// postproc is active the scene renders into the offscreen intermediate (so it
// can be graded as a whole); otherwise straight into the framebuffer (the
// default, canary-preserving path). RTT-resume (EndDrawTarget) routes through
// this so a mid-frame RndCam::TargetTex draw resumes into the right surface.
// ===========================================================================
wgpu::TextureView BandRnd::MainColorTarget() {
    // Tier 2: once the mid-frame venue composite has flushed onto the framebuffer,
    // the main pass renders into the FRAMEBUFFER (the graded venue is already
    // there). Any further mid-frame RTT resume after the flush must therefore
    // resume into mFrameView, not the now-stale intermediate.
    if (mPostProcFlushed)
        return mFrameView;
    if (!RB3PostProcDisabled() && RndPostProc::Current() && mIntermediateView && mRenderedToIntermediate)
        return mIntermediateView;
    return mFrameView;
}

// (Re)create the offscreen intermediate at w x h using mTargetFmt (RGBA8
// headless / BGRA8 windowed — NEVER hardcoded). usage RenderAttachment (the
// scene renders into it) | TextureBinding (the composite samples it). Recreate
// on size change.
void BandRnd::EnsureIntermediate(int w, int h) {
    if (!mGpuReady || w <= 0 || h <= 0) return;
    if (mIntermediateTex && mIntermediateView &&
        mIntermediateWidth == w && mIntermediateHeight == h) {
        return;  // already sized correctly
    }
    wgpu::TextureDescriptor td{};
    td.label = "RB3PostProcIntermediate";
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = mTargetFmt;   // matches the framebuffer the composite writes to
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    wgpu::Texture t = mGpu.Device().CreateTexture(&td);
    if (!t) return;
    mIntermediateTex = t;
    mIntermediateView = t.CreateView();
    mIntermediateWidth = w;
    mIntermediateHeight = h;
    // WASH-fix (Wave 8 A.S1) H2 probe: the composite intermediate is created at
    // the framebuffer format (mTargetFmt — RGBA8/BGRA8 UNORM, never a float HDR
    // format), so the scene renders into a per-channel [0,1] UNORM target. Any
    // hot (>1.0) venue lighting is clamped at the intermediate WRITE, before any
    // grade/ceiling can act — the mechanism H2 predicts feeds the mid-tone desat.
    // Log the format ONCE per (re)create so the unorm fact is on the record.
    static bool sWashPP = false, sWashPPInit = false;
    if (!sWashPPInit) { const char* e = getenv("RB3_WASH_PROBE"); sWashPP = (e && e[0] && e[0] != '0'); sWashPPInit = true; }
    if (sWashPP) {
        const char* fn = (mTargetFmt == wgpu::TextureFormat::RGBA8Unorm) ? "RGBA8Unorm"
                       : (mTargetFmt == wgpu::TextureFormat::BGRA8Unorm) ? "BGRA8Unorm"
                       : "other";
        fprintf(stderr, "[WASHPROBE] PP intermediate %dx%d fmt=%s (unorm=%d) — hot>1.0 clamped at write\n",
                w, h, fn, (mTargetFmt == wgpu::TextureFormat::RGBA8Unorm ||
                           mTargetFmt == wgpu::TextureFormat::BGRA8Unorm) ? 1 : 0);
    }
}

// Grade the intermediate onto `dst` (the framebuffer): a single fullscreen
// triangle (vs_fullscreen, no vbuf) running fs_postproc — ported verbatim from
// gfx/PostProcPass.cpp. No blend, no depth, LoadOp::Clear (full-screen
// overwrite — no Load reliance, web BGRA8 safe). Reads grade params from
// RndPostProc::Current() via the HX_NATIVE accessors. Bloom is v1-skipped:
// mBlackView is bound to bloomTex@3, so the screen-blend bloom term is a no-op.
bool RB3PostProcDisabled() {
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_PP_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return s != 0;
}

// W3.3-fix: RB3_PP_LUMA_CEILING=1 -> luminance-preserving highlight ceiling.
// Default-OFF; see RB3PostProc.h.
bool RB3PPLumaCeilingActive() {
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_PP_LUMA_CEILING"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return s != 0;
}

// WASH-fix (Wave 8 A.S2) FIX-H2: venue-scoped chroma-preserving composite
// grade. FLIPPED default-ON (Wave 8 coordinator sign-off, 2026-07-07: grey
// venue -> restored colored stage lighting, vlo fail-red 6/6 -> 0/5, flag-OFF
// was byte-identical). Opt out via RB3_PP_CHROMA_PRESERVE_OFF; the legacy
// opt-in name stays valid. See RB3PostProc.h.
bool RB3PPChromaPreserveActive() {
    static int s = -1;
    if (s < 0) {
        if (getenv("RB3_PP_CHROMA_PRESERVE_OFF"))              s = 0;   // opt-out wins
        else { const char* e = getenv("RB3_PP_CHROMA_PRESERVE"); s = (e && e[0] == '0') ? 0 : 1; }   // "=0" legacy disable kept; unset -> ON
    }
    return s != 0;
}

// Wave-13 Lane G: RB3_UI_POST_GRADE=1 -> generalize the mid-frame venue flush to
// the menu venue->UI boundary (UI draws ungraded over the graded venue).
// Default-ON since the Wave-14 coordinator E1 flip (hub focused-text contrast
// 1.95->2.20, song_select red-band fixed by the menu-boundary depth
// LoadOp::Load). Opt out with RB3_UI_POST_GRADE_OFF; legacy =0 disable kept.
bool RB3UIPostGradeActive() {
    static int s = -1;
    if (s < 0) {
        if (getenv("RB3_UI_POST_GRADE_OFF")) s = 0;
        else { const char* e = getenv("RB3_UI_POST_GRADE"); s = (e && e[0] == '0') ? 0 : 1; }
    }
    return s != 0;
}

// Menu-flush-pending latch (Wave-13 Lane G). Set by the game-side menu venue->UI
// boundary trigger right before Rnd::EndWorld(); consumed (read-and-cleared) by
// FlushPostProcMidFrame to select venueGrade=false. File-scope, single-threaded
// render path (same thread that runs the flush). No-op unless RB3_UI_POST_GRADE.
static bool sMenuUIFlushPending = false;
void RB3SetMenuUIFlushPending() { if (RB3UIPostGradeActive()) sMenuUIFlushPending = true; }
bool RB3ConsumeMenuUIFlushPending() {
    bool p = sMenuUIFlushPending;
    sMenuUIFlushPending = false;
    return p;
}

// Wave-14 U-CLEAN: flush-ONLY menu-UI post-grade seam (see RB3PostProc.h). Sets
// the menu-flush latch and drives the mid-frame venue grade DIRECTLY, WITHOUT the
// ClearDepthForOverlay else-branch depth-clear that produced the song_select red
// band. `rnd` is TheRnd (a BandRnd on native); the direct FlushPostProcMidFrame
// call consumes the latch at its top and early-returns when there is nothing to
// flush, so a menu dir with no graded venue is a safe no-op. Default-OFF: gated on
// RB3_UI_POST_GRADE via RB3UIPostGradeActive().
void RB3FlushMenuUIPostGrade(Rnd* rnd) {
    if (!RB3UIPostGradeActive() || !rnd) return;
    RB3SetMenuUIFlushPending();
    static_cast<BandRnd*>(rnd)->FlushPostProcMidFrame();
}

void BandRnd::RunPostProcComposite(wgpu::TextureView dst, bool venueGrade) {
    if (!mGpuReady || !dst || !mIntermediateView) return;
    RndPostProc* pp = RndPostProc::Current();
    if (!pp) return;

    EnsureQuadPipeline();
    if (!mQuadPostShader || !mPostProcUB) return;

    // --- Fill the uniform block (ported from PostProcPass::Run :260-303) ---
    PostProcUniforms uni{};
    const RndColorXfm& cxfm = pp->GetColorXfm();
    uni.contrast = cxfm.mContrast;
    uni.brightness = cxfm.mBrightness;
    uni.saturation = cxfm.mSaturation;
    uni.vignetteIntensity = pp->GetVignetteIntensity();
    const Hmx::Color& vc = pp->GetVignetteColor();
    uni.vignetteColor[0] = vc.red;   uni.vignetteColor[1] = vc.green;
    uni.vignetteColor[2] = vc.blue;  uni.vignetteColor[3] = vc.alpha;
    uni.chromaticOffset = pp->GetChromaticAberrationOffset();
    uni.chromaticSharpen = pp->GetChromaticSharpen() ? 1.0f : 0.0f;
    uni.posterLevels = pp->GetPosterLevels();
    uni.posterMin = pp->GetPosterMin();

    uni.levelInLo[0] = cxfm.mLevelInLo.red;   uni.levelInLo[1] = cxfm.mLevelInLo.green;
    uni.levelInLo[2] = cxfm.mLevelInLo.blue;  uni.levelInLo[3] = 0;
    uni.levelInHi[0] = cxfm.mLevelInHi.red;   uni.levelInHi[1] = cxfm.mLevelInHi.green;
    uni.levelInHi[2] = cxfm.mLevelInHi.blue;  uni.levelInHi[3] = 1;
    uni.levelOutLo[0] = cxfm.mLevelOutLo.red; uni.levelOutLo[1] = cxfm.mLevelOutLo.green;
    uni.levelOutLo[2] = cxfm.mLevelOutLo.blue; uni.levelOutLo[3] = 0;
    uni.levelOutHi[0] = cxfm.mLevelOutHi.red; uni.levelOutHi[1] = cxfm.mLevelOutHi.green;
    uni.levelOutHi[2] = cxfm.mLevelOutHi.blue; uni.levelOutHi[3] = 1;

    // BOOTRNG (Wave 11 A.S1, diagnosis-only): log the RESOLVED ColorXfm values the
    // native composite actually consumes (A2). RndPostProc::Current()'s NAME is
    // constant per boot on the gameplay path (world.pp rewritten in place), but its
    // grade params differ when BandDirector::Poll interps a different postproc pair
    // — this is the value that reaches pixels. Throttled per change + heartbeat.
    // Gated by RB3_BOOTRNG_PROBE; additive, no behaviour change.
    {
        static int sBRP = -1;
        if (sBRP < 0) { const char* e = getenv("RB3_BOOTRNG_PROBE"); sBRP = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (sBRP) {
            char pb[320];
            snprintf(pb, sizeof(pb),
                     "[BOOTRNG] PPRESOLVED pp=%s con=%.4f bri=%.4f sat=%.4f vig=%.4f "
                     "lvlInLo=(%.3f,%.3f,%.3f) lvlInHi=(%.3f,%.3f,%.3f) "
                     "lvlOutLo=(%.3f,%.3f,%.3f) lvlOutHi=(%.3f,%.3f,%.3f)",
                     pp->Name() ? pp->Name() : "<null>",
                     uni.contrast, uni.brightness, uni.saturation, uni.vignetteIntensity,
                     uni.levelInLo[0], uni.levelInLo[1], uni.levelInLo[2],
                     uni.levelInHi[0], uni.levelInHi[1], uni.levelInHi[2],
                     uni.levelOutLo[0], uni.levelOutLo[1], uni.levelOutLo[2],
                     uni.levelOutHi[0], uni.levelOutHi[1], uni.levelOutHi[2]);
            static std::string sPPLast; static int sPPHB = 0;
            std::string ppk(pb);
            if (ppk != sPPLast || (++sPPHB % 60) == 0) { fprintf(stderr, "%s\n", pb); sPPLast = ppk; }
        }
    }

    // V2 BLOOM. Run the threshold/blur/upsample mip chain on the intermediate
    // (the fully-rendered, pre-grade scene) and additive-blend its OutputView()
    // into bloomTex@3 (the shader branch at kRB3PostProcShaderSource's
    // `bloomIntensity > 0.0`). Mirrors gfx/PostProcPass.cpp:251-257 / :330-331
    // (DC3's path) but with two corrections for the rb3 backend:
    //
    //   THRESHOLD SCALE. RndPostProc::mBloomThreshold is in the Wii's pre-tonemap
    //   luminance scale (default 4.0; world.pp + subwayhangout.pp both ship 10.0),
    //   NOT the [0,1] normalized luma our composite operates in. PostProcPass only
    //   FLOORS it (max(thr,0.7)), so a raw 10.0 passes straight through and
    //   fs_bloom_threshold's `luma - 10 + knee` is negative for every SDR pixel →
    //   bloom NEVER fires (verified: world.pp raw thr=10 → zero visible bloom). And
    //   fs_bloom_threshold's soft knee is `knee = threshold*0.5`, so the EFFECTIVE
    //   onset is threshold*0.5 — a nominal 0.9 actually blooms everything above 0.45
    //   (washes the whole frame, verified). So we IGNORE the inflated Wii value and
    //   pass a FIXED normalized cutoff: kBloomThreshold 1.8 → onset 0.9, i.e. only
    //   the brightest ~few-% highlights bloom (ground-truth SP/venue frames bloom
    //   only the brightest ~2%, p99 luma ~0.75). Combined with the small additive
    //   blend (bloomColor.a, set below) this is a tight halo, never a wash/blowout.
    //
    // The mip chain records its own render passes into mEncoder; this runs AFTER
    // the main pass closed (EndFrame ends mPass before calling us) and BEFORE the
    // composite's BeginRenderPass below, so the intermediate is fully written and
    // the bloom textures are ready when the composite samples them.
    static const float kBloomThreshold = 1.8f;   // → fs_bloom_threshold onset ~0.9
    float bloomIntensity = std::min(pp->GetBloomIntensity(), 1.0f);
    float bloomThreshold = kBloomThreshold;
    wgpu::TextureView bloomView = mBlackView;   // inert default (branch won't sample)
    {
        // RB3_BLOOM_OFF: A/B isolation — disable ONLY the bloom term while keeping
        // the rest of the composite byte-identical (so a same-scene bloom-on vs
        // bloom-off diff measures exactly the bloom, not the grade/grain).
        static int s = -1;
        if (s < 0) { const char* e = getenv("RB3_BLOOM_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s) bloomIntensity = 0.0f;
    }
    // RB3_BLOOM_THRESH / RB3_BLOOM_SCALE: tuning overrides (sweep without rebuild).
    {
        const char* t = getenv("RB3_BLOOM_THRESH");
        if (t && t[0]) bloomThreshold = (float)atof(t);
        const char* sc = getenv("RB3_BLOOM_SCALE");
        if (sc && sc[0]) bloomIntensity *= (float)atof(sc);
    }
    if (bloomIntensity > 0.0f) {
        mBloom.Run(mEncoder, mIntermediateView, mIntermediateWidth, mIntermediateHeight,
                   bloomIntensity, bloomThreshold, pp->GetBloomColor(), mGpu);
        if (mBloom.HasOutput()) bloomView = mBloom.OutputView();
    }
    uni.bloomIntensity = bloomIntensity;
    const Hmx::Color& bc = pp->GetBloomColor();
    uni.bloomColor[0] = bc.red; uni.bloomColor[1] = bc.green;
    uni.bloomColor[2] = bc.blue;
    // bloomColor.a = additive blend factor for the composite (see the bloom branch
    // in kRB3PostProcShaderSource). Kept low so only true highlights produce a
    // visible halo and the dark background never washes. RB3_BLOOM_BLEND overrides.
    {
        // 0.02: at threshold-onset 0.9 the bloom output still has wide low-frequency
        // spread (the BloomPass mip chain blurs highlights broadly), so the additive
        // factor must stay small — verified the dark menu background lifts <1.5/255
        // (no wash) while bright text/album-art/SP highlights gain a visible halo.
        float blend = 0.02f;
        const char* b = getenv("RB3_BLOOM_BLEND");
        if (b && b[0]) blend = (float)atof(b);
        uni.bloomColor[3] = blend;
    }

    uni.time = (float)mFrameCount;
    // W3.3-fix: RB3_PP_LUMA_CEILING toggle for the highlight-ceiling guard
    // below the knee (see rb3_postproc.wgsl.inc fs_postproc). Default 0.0 ->
    // byte-identical per-channel path (today's shipped behavior).
    uni.lumaCeilingActive = RB3PPLumaCeilingActive() ? 1.0f : 0.0f;
    // WASH-fix (Wave 8 A.S2) FIX-H2: venue-scoped chroma preservation. Only the
    // venue-backdrop composite (venueGrade) is eligible; flag default-OFF ->
    // chromaPreserveActive 0.0 -> the shader path is byte-identical.
    uni.chromaPreserveActive = RB3PPChromaPreserveActive() ? 1.0f : 0.0f;
    uni.venueGrade = venueGrade ? 1.0f : 0.0f;
    // V2 NOISE GRAIN. RB3's postproc noise is a TILED NOISE TEXTURE (mNoiseMap +
    // mNoiseBaseScale tiling, midtone-overlay blended) — a SUBTLE film grain on
    // the Wii. v1 zeroed it because the procedural hash fallback at RB3's real
    // intensity (~3.0) washed the frame gray. We now bind the real bitmap and
    // attenuate the deviation (kNoiseGain, in the shader) so it is grain, not a
    // wash. A postproc with intensity!=0 but no bitmap falls back to the
    // (same-attenuated) procedural path. With intensity==0 the branch is skipped
    // entirely → still an exact identity passthrough for neutral env postprocs,
    // keeping the no-postproc/zero-noise canary pixel-clean.
    RndTex* noiseMap = pp->GetNoiseMap();
    wgpu::TextureView noiseView;
    if (noiseMap) noiseView = UploadRndTexIfNeeded(mGpu, noiseMap);
    if (noiseView) {
        uni.noiseHasMap = 1.0f;
        const Vector2& ns = pp->GetNoiseBaseScale();
        uni.noiseScaleX = ns.x;
        uni.noiseScaleY = ns.y;
    } else {
        uni.noiseHasMap = 0.0f;       // procedural fallback (still attenuated)
        uni.noiseScaleX = 0.0f;
        uni.noiseScaleY = 0.0f;
        noiseView = mBlackView;       // a valid view must be bound regardless
    }
    uni.noiseIntensity = pp->GetNoiseIntensity();
    uni.noiseMidtone = pp->GetNoiseMidtone() ? 1.0f : 0.0f;
    // RB3_NOISE_OFF: A/B isolation — disable ONLY the grain term while keeping
    // the rest of the composite byte-identical (so a same-scene grain-on vs
    // grain-off diff measures exactly the grain, not scene motion / the grade).
    {
        static int s = -1;
        if (s < 0) { const char* e = getenv("RB3_NOISE_OFF"); s = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s) uni.noiseIntensity = 0.0f;
    }

    // flicker disabled (separate follow-up).
    uni.flickerMul = 1.0f;

    mGpu.Queue().WriteBuffer(mPostProcUB, 0, &uni, sizeof(uni));

    // --- Pipeline (cached): format mTargetFmt, no blend, no depth ---
    if (!mQuadPostPipeline) {
        wgpu::ColorTargetState ct{};
        ct.format = mTargetFmt;
        ct.writeMask = wgpu::ColorWriteMask::All;   // no blend (opaque overwrite)

        wgpu::FragmentState frag{};
        frag.module = mQuadPostShader;
        frag.entryPoint = "fs_postproc";
        frag.targetCount = 1;
        frag.targets = &ct;

        wgpu::RenderPipelineDescriptor pd{};
        pd.layout = mQuadPostPL;
        pd.vertex.module = mQuadPostShader;
        pd.vertex.entryPoint = "vs_fullscreen";
        pd.fragment = &frag;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pd.depthStencil = nullptr;   // composite has no depth attachment
        pd.multisample.count = 1;    // rb3 backend is single-sampled
        mQuadPostPipeline = mGpu.Device().CreateRenderPipeline(&pd);
    }
    if (!mQuadPostPipeline) return;

    wgpu::BindGroupEntry bge[5] = {};
    bge[0].binding = 0; bge[0].textureView = mIntermediateView;
    bge[1].binding = 1; bge[1].sampler = mSampler;
    bge[2].binding = 2; bge[2].buffer = mPostProcUB; bge[2].offset = 0; bge[2].size = sizeof(PostProcUniforms);
    bge[3].binding = 3; bge[3].textureView = bloomView;    // V2 bloom output (or black when inactive)
    bge[4].binding = 4; bge[4].textureView = noiseView;    // V2 tiled noise (or black fallback)
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mQuadPostBGL;
    bgd.entryCount = 5;
    bgd.entries = bge;
    wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = dst;
    colorAtt.loadOp = wgpu::LoadOp::Clear;     // full-screen overwrite (no Load)
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.clearValue = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor rp{};
    rp.label = "BandPostProcComposite";
    rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
    rp.depthStencilAttachment = nullptr;

    wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
    pass.SetPipeline(mQuadPostPipeline);
    pass.SetBindGroup(0, bg, 0, nullptr);
    pass.Draw(3);
    pass.End();

    // RB3_RENDER_DBG: prove the composite fires ONLY on postproc screens
    // (song_select / etched), NEVER on plain gameplay or main_hub. Log on every
    // CHANGE of the active postproc object (name + full grade) plus a periodic
    // heartbeat, so the verify can confirm B+W_film02 is the active grade.
    if (getenv("RB3_RENDER_DBG")) {
        static RndPostProc* sLastPP = nullptr;
        RndCam* cur = RndCam::sCurrent;
        const char* camName = (cur && cur->Name()) ? cur->Name() : "<none>";
        if (pp != sLastPP) {
            sLastPP = pp;
            // Log the active postproc + its grade whenever Current() changes —
            // proves the composite fires ONLY on postproc-active screens, and
            // which grade (name + sat/contrast/levels/vignette) is applied.
            fprintf(stderr,
                "[RB3_RENDER_DBG] postproc composite active f%d pp='%s' cam=%s sat=%.1f "
                "contrast=%.1f bright=%.1f vignette=%.2f outLo=(%.3f,%.3f,%.3f) %dx%d "
                "noise[int=%.2f midtone=%.0f hasMap=%.0f scale=(%.1f,%.1f)] "
                "bloom[int=%.2f thresh=%.2f color=(%.2f,%.2f,%.2f) raw=%.2f rawThr=%.2f]\n",
                mFrameCount, pp->Name() ? pp->Name() : "?", camName,
                uni.saturation, uni.contrast, uni.brightness, uni.vignetteIntensity,
                uni.levelOutLo[0], uni.levelOutLo[1], uni.levelOutLo[2],
                mIntermediateWidth, mIntermediateHeight,
                uni.noiseIntensity, uni.noiseMidtone, uni.noiseHasMap,
                uni.noiseScaleX, uni.noiseScaleY,
                uni.bloomIntensity, bloomThreshold,
                uni.bloomColor[0], uni.bloomColor[1], uni.bloomColor[2],
                pp->GetBloomIntensity(), pp->GetBloomThreshold());
        }
    }
}
