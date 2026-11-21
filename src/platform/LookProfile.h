#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Look profile: the one switch between the original hardware's look and the
// native port's polish (DC3 only).
//
//   DC3_LOOK=enhanced   (default, also when unset) native polish ON
//   DC3_LOOK=retail     the original's behaviour, for parity measurement
//                       against Xenia / retail footage
//
// Any other value warns once and falls back to enhanced. The variable is read
// ONCE (first call to Look()), registered in NativeCompatFlags.classification.json.
//
// Only the DC3 retail model (rndshape::kDC3RetailModel) reads it. rb3-xenon
// (DC3 shape header, not the retail model) and rb3 (RB3Wii shape,
// kDC3RetailModel = false) get LookProfile::Native and never read the variable.
// With Native, neither Retail() nor Enhanced() is true, so an enhancement
// written as `if (Look().Enhanced())` and a faithful branch written as
// `if (Look().Retail())` both leave those consumers exactly as they render
// today. Keep it that way: never key an rb3-visible change on
// `!Look().Retail()`.
//
// Hanging an enhancement here: add a bool to LookSettings, set it in
// LookProfile.cpp from the profile (Enhanced -> on), document it below with the
// original behaviour it departs from, and read it at the draw site. One switch,
// many enhancements; do not add a per-feature env var. The exceptions are COST
// knobs, not looks: MILO_RENDER_SCALE (renderScaleExplicit below), so a device
// that cannot carry supersampling keeps the rest of the look, and
// MILO_OUTPUT_MAX_ROWS (outputMaxRowsExplicit), the output resolution's cap.
// ─────────────────────────────────────────────────────────────────────────────

namespace milo {

enum class LookProfile : unsigned char {
    Native,    // consumer has no look switch (rb3, rb3-xenon): unchanged renderer
    Retail,    // DC3: the original hardware's behaviour
    Enhanced,  // DC3: native polish (default)
};

struct LookSettings {
    LookProfile profile = LookProfile::Native;

    bool Retail() const { return profile == LookProfile::Retail; }
    bool Enhanced() const { return profile == LookProfile::Enhanced; }

    // ---- texture sampling (sharpness lane) -----------------------------------
    // Levels of a regular bitmap texture uploaded, base included (0 = the whole
    // authored chain). Retail and Native: the consumer's original rule,
    // rndshape::kBitmapMaxMipLevels (DC3's DxTex::SyncBitmap uploads
    // min(NumMips()+1, 2); rb3-xenon's 0 = all). Enhanced: see SettingsFor.
    int bitmapMipLevels = 0;
    // Sampler max anisotropy for material textures (diffuse/normal/spec/...).
    // The original sets none (fetch-constant aniso_filter 0); Enhanced raises it.
    unsigned short maxAnisotropy = 1;
    // Text and HUD-overlay textures: highest level sampled, base included
    // (0 = no limit). Enhanced uploads whole chains, so this keeps glyphs at
    // the original's min(NumMips()+1, 2) -- see MaterialSetup.cpp.
    int uiMipLevels = 0;
    // LOD bias added to world material texture samples, as a function of the
    // render scale the pass draws at (MaterialLodBiasAt): lodBiasScale1 at 1x,
    // lodBiasScale2 at 2x, linear in log2(scale) between. The value reaches
    // the shader through the scene uniforms (SceneUniforms::materialLodBias,
    // written per pass), so a scale change at run time -- a window resized
    // across a step -- compiles no pipeline. The original's fetch constants
    // carry lod_bias 0.
    float lodBiasScale1 = 0.0f;
    float lodBiasScale2 = 0.0f;
    // World material draws read that bias from the scene uniforms
    // (PipelineKey::lodBias), whatever its value. Enhanced sets it so the
    // inspection tokens below can move the bias live even while the rule is 0
    // at every scale; retail and Native leave it off, so their pipelines
    // sample exactly as before.
    bool materialLodBias = false;
    // Inspection ({rnd native_look lod_bias_...}): this bias at every scale
    // while lodBiasInspected; the rule above otherwise.
    bool lodBiasInspected = false;
    float lodBiasInspect = 0.0f;
    // Cube maps upload their whole authored mip chain, as DC3's
    // DxCubeTex::Sync creates them (numMips = mNumMips + 1). Native's one
    // level was a port shortcut; both DC3 profiles fix it, Native keeps it.
    bool cubeMipChain = false;

    // ---- supersampling ---------------------------------------------------------
    // Internal render scale: the frame is drawn at this times its output size
    // and box-filtered back down where it leaves the renderer (present,
    // readback; GpuDeviceDesc::renderScale). 1 = off, the original's one
    // sample per pixel (its world draws were not multisampled either; native
    // keeps its 4x MSAA under every profile). Desktop only (the web build
    // ignores it). Per adapter class, because the cost is the main pass's fill
    // times scale^2: renderScale on a discrete GPU, renderScaleIntegrated on
    // anything else (integrated: a handheld's main pass is ~2 ms at 1x).
    // These are DEFAULTS, and a default applies only when the engine presents
    // to a window: a headless run (tests, gates, screenshot / readback-only
    // captures) draws at 1x unless it asks (renderScaleExplicit). The default
    // is also capped so the drawn frame stays within 3840x2160
    // (ResolveRenderScale, gfx/GpuDevice.h).
    float renderScale = 1.0f;
    float renderScaleIntegrated = 1.0f;
    // MILO_RENDER_SCALE=<float>: an explicit scale, under either DC3 profile,
    // headless included, any adapter class, not capped; clamped to [1, 2].
    // 0 = unset (the profile's default above). It keeps the rest of the look:
    // MILO_RENDER_SCALE=1 is the enhanced look without supersampling (a weak
    // discrete card), 1.5 or 2 tries supersampling on a handheld. Read once
    // with DC3_LOOK; DC3 retail model only -- rb3 / rb3-xenon never read it.
    float renderScaleExplicit = 0.0f;

    // ---- output resolution -------------------------------------------------------
    // The frame is drawn at its output's own resolution: the window's size (or
    // headless MILO_WIDTH x MILO_HEIGHT) fitted to 16:9, with UI, viewports and
    // the post chain's measures laid out in the 1280x720 virtual resolution
    // (GpuDeviceDesc::outputFollowsTarget; also needs the shape's
    // rndshape::kOutputFollowsTarget). The original drew a 1280x720 frame and
    // the scaler stretched it to the display; Retail keeps that frame, scaled
    // into the window by the present path. Enhanced: on.
    bool outputFollowsTarget = false;
    // Output height cap, in rows, on a non-discrete adapter presenting to a
    // window (GpuDeviceDesc::outputMaxRowsIntegrated; 0 = none): a handheld's
    // iGPU draws at most 1920x1080 and the present path scales that into a
    // larger panel. Enhanced: 1080. Discrete adapters are not capped. The web
    // build, which cannot tell a discrete adapter, uses it as its default.
    int outputMaxRowsIntegrated = 0;
    // The web build's cap on an adapter without texture-compression-bc
    // (GpuDeviceDesc::outputMaxRowsNoBc, WebOutputRowCap): every phone and
    // tablet GPU, and what ?gpu_profile=mobile|bare mask. Enhanced: 720 until
    // a phone is measured. The knob beats it.
    int outputMaxRowsNoBc = 0;
    // The second COST knob: MILO_OUTPUT_MAX_ROWS=<rows> overrides the cap in
    // both directions, on any adapter, headless included: 0 = no cap (try
    // 1440 on the handheld with 1440), 720 = a 1280x720 frame anywhere.
    // -1 = unset (the default above). Read once with DC3_LOOK; DC3 retail
    // model only. Meaningless under Retail, whose frame is 1280x720 anyway.
    int outputMaxRowsExplicit = -1;

    // ---- post chain (np-gfx-post) ----------------------------------------------
    // np-gfx-post round 10: the post chain's world grade only, i.e. the post
    // run before the HUD and UI draw (WgpuRnd::FlushPostProcessingForOverlay,
    // PostProcPass::SetWorldGrade). A post that runs over the UI (menus graded
    // at EndDrawing) stays retail, so UI, HUD and text never change. Engine
    // doc §37.11 has the measurements and costs.
    //   postPrecision    the bloom sets in RGBA16Float, still saturated at 1
    //                    after every pass as retail's 8-bit EDRAM resolve
    //                    saturates them (A8R8G8B8, NgPostProc::
    //                    BloomTextureSet), and keeping that resolve's dead zone
    //                    (8-bit rounding drops whatever a pass leaves under half
    //                    a level: a dim surface's own glow) as a smooth knee,
    //                    so the steps go and the energy stays; a static sub-LSB
    //                    dither on the composite's 8-bit write. Retail: 8-bit
    //                    sets, no dither.
    //   postBloomFilter  tent (not box) downsample into the bloom sets; the
    //                    streak arm's w/16 set from all 256 texels of its block
    //                    (retail's bloom.ps reads 16 of them, 4 bilinear taps
    //                    at +-2 texels); a cubic B-spline (not bilinear)
    //                    upsample of the plain arm's coarse sets (combined once
    //                    at w/4) and of the glare arm's set.
    //   postDofFilter    depth of field: tent downsample, B-spline upsample of
    //                    the blur in the full-resolution composite, the 8 blur
    //                    taps scaled so the total blur radius (second moment)
    //                    stays retail's. Retail: box, bilinear.
    bool postPrecision = false;
    bool postBloomFilter = false;
    bool postDofFilter = false;
    // np-look23 (c): the depth of field's blur gathers by depth (DofPass::Run
    // `depthWeighted`, with postDofFilter): each downsample tap weighs into the
    // quarter-size blur by its blur factor squared, read from a half-size
    // factor pre-pass, so an in-focus figure adds nothing to the blurred
    // background beside it. (A depth load per tap instead cost +0.84 ms at
    // 1920x1080 on a Radeon 890M; the half-size factor +0.25 ms.) The original's
    // downsample_4x is depth-blind: the figure's colour spreads ~3 quarter
    // texels into the background, a bright fringe around every in-focus
    // figure in a close-up (~12 px at 720; Xenia and the hardware show it).
    // Retail and Native: the depth-blind blur. Inspection:
    // {rnd native_look dof_depth_weight_on|_off|_rule}.
    bool postDofDepthWeight = false;
    // np-look23 (a): DC3's post noise map (noise_mono.tex) re-tiled so each
    // texel covers whole OUTPUT rows, read at one fixed place in the texel
    // (gfx/NoiseRows.h): the same fine lines on every row at every size and
    // render scale, at the authored mean contrast. The original ties the map
    // to the screen: 768 map rows down any screen (base scale (1, 3) of a
    // 256-row map) under a linear filter, so the blend of neighbouring rows
    // drifts down the screen -- a 2.1-2.8 px line pattern under a slow beat
    // (a 15-row beat at the Xbox's own 720 rows, which Xenia shows too).
    // Retail and Native: that tiling, beat included. Needs the shape's
    // capability as well (rndshape::kNoiseSnapsToOutputRows: rb3-xenon has it
    // false). Inspection: {rnd native_look noise_rows_on|_off|_rule}.
    bool noiseSnapsToOutputRows = false;

    // ---- shading (np-gfx item 18 c) ------------------------------------------
    // Specular antialiasing on the scene's per-pixel retail lighting (DC3's
    // standard, skin and hair specular): the Phong exponent widened by the
    // normal's change over the pixel (standard_wgsl.inc specularAAPower), so a
    // highlight narrower than a pixel stops blinking as the camera moves. The
    // lobes are normalised, so the light they reflect stays; only the peak of
    // a sub-pixel highlight comes down. Scene materials only: written by the
    // DC3 shape's scene lighting into the scene uniforms and read only by the
    // per-pixel retail lighting, which HUD and text never take. Retail: the
    // shipped exponent, per pixel as the 360 shades it.
    bool specularAA = false;

    // ---- alpha-tested cutouts (np-look26 d) --------------------------------------
    // The enhanced cutout (MaterialCutCoverage == Sharpened: hair, crowd
    // clothing, palms, a SrcAlpha floor line) in a 4x multisampled pass takes
    // the original's alpha test at each of the pass's four samples, and the
    // sample mask is the four results (standard_wgsl.inc cutoutSampleMask).
    // Each sample reads the diffuse alpha at its own position, one bilinear
    // fetch at the level the original's sampler reads for the pixel (two
    // levels at most, no anisotropy, no look LOD bias), times
    // the same material and vertex alpha the original's test multiplies in.
    // That is retail's cut supersampled 4x: nothing passes that retail's test
    // would not pass at that point, so a fade authored under the cut stays
    // invisible, and a strand thinner than a texel of the averaged mip (which
    // the 16x-anisotropic, whole-chain sample averages under the threshold and
    // dots out) covers the part of the pixel it crosses. Off: the landed
    // coverage (cutoutCoverage, the tested alpha sharpened to the pixel
    // footprint). World draws only, through the scene block (SceneUniforms::
    // cutoutSampleTest, 0 in the overlay phase), in pipelines that take the
    // LOD bias (text and HUD never do), with a diffuse texture, 4 samples.
    // Retail and Native: off (and no retail pipeline is fs_cutout).
    // np-look26 f: under that test the default is mode E (CutoutSampleMode):
    // where retail's level resolves the alpha (lod <= 1: palms, fences up
    // close) the per-sample mask alone (B, as smooth as the landed coverage);
    // where the texture is minified past retail's level 1 (hair at a
    // distance) retail's test at the pixel centre first, so every pixel
    // retail draws is drawn solid, and the per-sample mask only where the
    // centre fails (C; B alone was faithful but noisy there: four point
    // samples of sub-pixel strand alpha). Round 2 (np-look26 e) shipped C as
    // the default; C stays selectable by token. Inspection:
    // {rnd native_look hair_edge_on|hair_edge_off|hair_edge_rule}, and
    // hair_edge_b|hair_edge_c|hair_edge_e|hair_edge_centre for the modes.
    // (Item 26's boost of the tested alpha by mip level, rejected: it drew
    // content authored under the cut, dclive's PeakLine floor fades.)
    bool cutoutSampleTest = false;
};

// The cut-out modes the scene block carries (SceneUniforms::cutoutSampleTest,
// a float holding one of these; standard_wgsl.inc cutoutSampleMask). Only
// kCutoutModeDefault is the look's: the others are inspection tokens
// (np-look26 e), kept so one held frame can be measured under each.
//   0 off: the landed sharpened coverage (cutoutCoverage).
//   1 B: retail's test at each of the four samples, the mask the four results.
//   2 C: retail's test at the pixel centre first; pass = full mask (everything
//        retail draws is drawn solid), fail = B's mask (a fringe only where
//        retail's test passes at a sample position).
//   3 E: B where retail's level resolves the alpha (unclamped lod <= 1, at
//        most about one texel per pixel), C above that.
//   4 centre only (diagnostic): full mask if the centre passes, else nothing;
//        retail's cut shape.
enum CutoutSampleMode : int {
    kCutoutModeOff = 0,
    kCutoutModeB = 1,
    kCutoutModeC = 2,
    kCutoutModeE = 3,
    kCutoutModeCentre = 4,
};
constexpr int kCutoutModeDefault = kCutoutModeE;  // np-look26 f (was C in e)

// The value the scene block carries (SceneUniforms::cutoutSampleTest): 0
// while the look has no cutoutSampleTest, else kCutoutModeDefault, or the
// mode an inspection token chose (SetLookCutoutModeForInspection).
float CutoutSampleTestFor(const LookSettings &look);

// MILO_RENDER_SCALE's value -> LookSettings::renderScaleExplicit: 0 for unset,
// empty or not a positive number; else clamped to [1, 2]. *bad is set when a
// non-empty value was not understood (the caller warns).
float ParseRenderScaleKnob(const char *value, bool *bad = nullptr);

// MILO_OUTPUT_MAX_ROWS's value -> LookSettings::outputMaxRowsExplicit: -1 for
// unset, empty or not a whole number >= 0; else the number (0 = no cap).
// *bad is set when a non-empty value was not understood (the caller warns).
int ParseOutputRowsKnob(const char *value, bool *bad = nullptr);

// The process-wide settings, resolved once.
const LookSettings &Look();

const char *LookProfileName(LookProfile p);

// The bitmapMipLevels the look the process started in carries: the level
// count its regular textures were uploaded with (Tex_Wgpu reads
// Look().bitmapMipLevels at upload; textures are not re-uploaded on a look
// switch). SetLookProfileForInspection does not move it; ScopedLookOverride
// sets it to its profile's value for the scope (a test scope stands for a
// process started in that look) and restores it.
int StartupBitmapMipLevels();
// np-look26 f: the highest level count a material sampler may read under
// `look` after a look switch (0 = no clamp; ApplyLookSampling sets lodMaxClamp
// to one less): `look`'s bitmapMipLevels when it caps the upload (> 0) and
// differs from the startup look's, else 0. So after {rnd native_look retail}
// in a run started enhanced (whole chains uploaded), world samplers, and
// retail's text and HUD samplers (retail has no uiMipLevels), read retail's
// two levels, as retail's textures hold. A run that never switches looks gets
// 0 for every look and consumer: no sampler changes. Not a look departure, no
// LookSettings bool, and it keys on the level counts, never on the profile.
int LookSwitchTexLevels(const LookSettings &look, int startupBitmapMipLevels);

// Inspection (dc3's `{rnd native_look retail|enhanced}`): switch between the
// two DC3 profiles at run time, so an A/B takes the same frozen frame. Ignored
// (returns false) for a consumer whose profile is Native. Textures keep the
// level count they were uploaded with: switched to retail from a run started
// enhanced, world, text and HUD samplers are clamped to retail's two levels
// (LookSwitchTexLevels), so the frame has retail's cut and sharpness. The
// reverse cannot be made faithful: a run started retail uploaded two levels,
// so after {rnd native_look enhanced} minified textures read at most level 1,
// sharper and more aliased than an enhanced launch (only a re-upload would fix
// it; there is none). Compare against an enhanced launch for that direction.
// Enhanced's text and HUD samplers keep their class rule (uiMipLevels) either
// way.
bool SetLookProfileForInspection(LookProfile p);
// The same for the post chain's fields only (postPrecision, postBloomFilter,
// postDofFilter), the rest of the profile untouched: an A/B of the post look
// without the other enhancements (alpha-cut coverage) changing too.
bool SetLookPostForInspection(bool enhanced);
// {rnd native_look specular_aa_on|specular_aa_off}: specularAA alone, so one
// frozen frame can be measured both ways (shimmer, GPU cost). False under the
// Native profile.
bool SetLookSpecularAAForInspection(bool on);
// Whether world material draws take a LOD bias at all under `look`
// (PipelineKey::lodBias): the look reads the bias (materialLodBias), or the
// rule is non-zero at some scale. False for the retail and Native looks, whose
// pipelines then sample exactly as before.
bool LookTakesLodBias(const LookSettings &look);
// The world material LOD bias for a pass drawn at `scale` (the frame's render
// scale; 1 for a texture target, which is never supersampled). See
// LookSettings::lodBiasScale1.
float MaterialLodBiasAt(const LookSettings &look, float scale);
// The bias a pass's scene block carries (SceneUniforms::materialLodBias): the
// rule at the frame's render scale, or at 1 for a texture target, which is
// drawn at its own size and never box-filtered down.
float PassMaterialLodBias(const LookSettings &look, bool toTextureTarget, float frameRenderScale);
// {rnd native_look lod_bias_0|lod_bias_m05|lod_bias_m1|lod_bias_rule}: one bias
// at every scale, or back to the rule, so one frozen frame is measured under
// each. Takes effect on the next frame (a uniform, not a pipeline). False
// under the Native profile. Only a run that started in a look that takes a
// bias (enhanced) made its world pipelines biasable; in a run started retail
// this changes nothing on screen.
bool SetLookLodBiasForInspection(bool useRule, float bias);
// {rnd native_look aniso_1|aniso_4|aniso_16|aniso_rule}: the world samplers'
// max anisotropy (ApplyLookSampling; text and HUD never take it), or 0 for the
// profile's own value, so one run measures the GPU cost at each. Takes effect
// on the next frame through new samplers and bind groups (inspection only).
// False under the Native profile.
bool SetLookAnisotropyForInspection(unsigned short maxAnisotropy);
// {rnd native_look noise_rows_on|noise_rows_off|noise_rows_rule}: the post
// noise map's output-row re-tiling (noiseSnapsToOutputRows) on, off, or the
// profile's own value (mode 1 / 0 / -1), so one frozen frame is measured both
// ways. Takes effect on the next frame (uniforms only). False under the Native
// profile.
bool SetLookNoiseRowsForInspection(int mode);
// {rnd native_look dof_depth_weight_on|dof_depth_weight_off|dof_depth_weight_rule}:
// postDofDepthWeight on, off or the profile's (1 / 0 / -1). Takes effect on
// the next frame; the first switch on in a run that started retail compiles
// four pipelines. Only reaches the screen with postDofFilter on. False under
// the Native profile.
bool SetLookDofDepthWeightForInspection(int mode);
// {rnd native_look hair_edge_on|hair_edge_off|hair_edge_rule}: cutoutSampleTest
// on (the default mode), off, or the profile's own value (mode 1 / 0 / -1), so
// one frozen frame is measured both ways; each clears a mode token. A uniform:
// takes effect on the next frame and compiles nothing. False under the Native
// profile.
bool SetLookCutoutSampleTestForInspection(int mode);
// {rnd native_look hair_edge_b|hair_edge_c|hair_edge_e|hair_edge_centre}:
// cutoutSampleTest on with that CutoutSampleMode (1..4) in place of the
// default, until another hair_edge token. False under the Native profile or
// for a mode outside 1..4.
bool SetLookCutoutModeForInspection(int mode);
// {rnd native_look refl_cull_force|refl_cull_flip|refl_cull_rule}: the cull a
// WorldReflection's mirrored redraw gives its materials (ReflectionCull,
// Mesh_Wgpu.cpp): force = every material culls Backwards (DC3's CheckForceCull),
// flip = the material's own cull reversed and two-sided kept two-sided (the
// pre-item-20 native rule), rule = the shape's constant
// (rndshape::kReflectionForcesReverseCull). Inspection only: one frozen frame
// compared both ways. Not tied to the look profile.
void SetReflectionCullForInspection(int mode);  // -1 rule, 0 flip, 1 force
int ReflectionCullInspection();
// One post field at a time (the per-item cost A/B: each enhancement against
// retail in the same run); the fields not named are set retail.
bool SetLookPostFieldsForInspection(bool precision, bool bloomFilter, bool dofFilter);

// Tests only: replace the resolved settings for the lifetime of the object
// (nests; restores the previous settings on destruction). Not thread-safe.
class ScopedLookOverride {
public:
    explicit ScopedLookOverride(LookProfile p);
    ~ScopedLookOverride();
    ScopedLookOverride(const ScopedLookOverride &) = delete;
    ScopedLookOverride &operator=(const ScopedLookOverride &) = delete;

private:
    LookSettings mPrev;
    int mPrevStartupBitmapMipLevels;
};

}  // namespace milo
