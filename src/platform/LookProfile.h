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

#include <cstddef>
#include <cstdint>

namespace milo {

enum class LookProfile : unsigned char {
    Native,    // consumer has no look switch (rb3, rb3-xenon): unchanged renderer
    Retail,    // DC3: the original hardware's behaviour
    Enhanced,  // DC3: native polish (default)
};

// ---- adapter classes (np-look49) --------------------------------------------
// The look's per-adapter rules (the depth of field's near field and near
// spread, the texture pass's MSAA and size) are decided by one of three
// classes. Nothing that sizes the frame reads it: render scale, the output row
// cap and the thermal governor keep GpuDevice::IsDiscreteGpu().
//   Discrete: the adapter reports DiscreteGPU (an Intel Mac's Radeon too).
//   Apple: integrated, vendor 0x106B, on Dawn's Metal backend (Apple
//     silicon; Dawn reports it IntegratedGPU). Through MoltenVK (the Vulkan
//     backend) it is not Apple; nor is an Intel Mac's Intel graphics (0x8086),
//     nor a CPU or unknown-type adapter whatever its vendor.
//   OtherNonDiscrete: everything else (integrated, CPU, unknown): the rules
//     exactly as the one non-discrete class had them.
// np-look56, the web build: a browser reports no adapter type, vendor id or
// backend (emdawnwebgpu's wgpuAdapterGetInfo gives Unknown, 0 and WebGPU), so
// there the class is decided from the browser's GPUAdapterInfo instead
// (LookAdapterFacts::web): vendor "nvidia" on an adapter that is not a
// fallback adapter is Discrete; vendor "apple" with texture-compression-bc
// (an Apple silicon Mac; an iPhone or iPad has no BC), not a fallback, is
// Apple only while the Apple-in-a-browser constant (kAppleInBrowser,
// LookProfile.cpp) is on, and OtherNonDiscrete while it is off; anything else
// ("amd", "intel", "arm", "qualcomm", empty, unknown, any fallback adapter)
// is OtherNonDiscrete. An AMD or Intel string does not tell a desktop card
// from a handheld's integrated GPU. The vendor is compared exactly,
// case-sensitively: the WebGPU specification gives a normalised lower-case
// identifier, and anything else is not one ("NVIDIA" is OtherNonDiscrete).
// As for the native facts, nothing that sizes the frame reads it: the web
// build's IsDiscreteGpu() stays false, render scale 1, the row cap as before.
// An enum class so a bool can never be passed where a class is wanted.
enum class LookAdapterClass : unsigned char {
    Discrete,
    Apple,
    OtherNonDiscrete,
};

// The adapter's reported type, as plain facts (no wgpu types, so tests can
// walk the classification without a device).
enum class LookAdapterType : unsigned char {
    Discrete,
    Integrated,
    Cpu,
    Unknown,
};

constexpr uint32_t kAppleGpuVendorId = 0x106B;

// What the class is decided from, recorded by GpuDevice when it gets its
// adapter. backendName is for the log line only (a string literal).
// web: the facts are a browser's (GpuDevice_Web.cpp), and the class is
// decided from webVendor, webHasBC and webFallback alone (type, vendorId and
// metalBackend carry what emdawnwebgpu reports, which says nothing). The
// strings are copied in (SetLookAdapterWebStrings), truncated to fit, so the
// facts stay plain data that copies.
constexpr int kLookAdapterWebStringSize = 32;
struct LookAdapterFacts {
    LookAdapterType type = LookAdapterType::Unknown;
    uint32_t vendorId = 0;
    bool metalBackend = false;
    const char *backendName = "unknown";
    bool web = false;
    char webVendor[kLookAdapterWebStringSize] = {};        // GPUAdapterInfo.vendor
    char webArchitecture[kLookAdapterWebStringSize] = {};  // .architecture (log line only)
    bool webHasBC = false;                                  // texture-compression-bc
    bool webFallback = false;                               // .isFallbackAdapter
};
// Copies vendor / architecture (len bytes each, not NUL-terminated; null =
// empty; SIZE_MAX = NUL-terminated) into f.webVendor / f.webArchitecture,
// truncated to fit.
void SetLookAdapterWebStrings(LookAdapterFacts &f, const char *vendor, size_t vendorLen, const char *architecture,
                              size_t architectureLen);

// The classification, a pure function of the three facts.
LookAdapterClass ClassifyLookAdapter(LookAdapterType type, uint32_t vendorId, bool metalBackend);
// np-look56: a browser's adapter (see above). appleInBrowser is the
// Apple-in-a-browser constant's value, an argument so tests walk both.
LookAdapterClass ClassifyWebLookAdapter(const char *vendor, bool hasBC, bool fallback, bool appleInBrowser);
// kAppleInBrowser (LookProfile.cpp): false; the comment there says why.
bool LookAppleInBrowser();
// The facts' class: the web rule with kAppleInBrowser when f.web, else the
// native rule.
LookAdapterClass ClassifyLookAdapter(const LookAdapterFacts &f);
// "discrete GPU", "Apple GPU (not a discrete GPU)", "not a discrete GPU".
const char *LookAdapterClassName(LookAdapterClass c);
// "discrete", "integrated", "cpu", "unknown".
const char *LookAdapterTypeName(LookAdapterType t);

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
    // np-look34: LOD bias added to text and HUD-overlay (uiClass) diffuse
    // samples, beside uiMipLevels: the text sampling rule is these two numbers.
    // A uniform (SceneUniforms::uiLodBias / uiArtLodBias, written per pass),
    // so a token switches it with no pipeline compiled; uiTakesLodBias says
    // whether uiClass pipelines read it (PipelineKey::uiBias). At render scale
    // above 1 the output-pixel path scales its gradients by 2^bias
    // (standard_wgsl.inc diffuseSample). uiLodBiasGlyphsOnly: only glyph pages
    // (isTextMesh) take it; other uiClass art (flashcard pictograms, icons,
    // bars) reads uiArtLodBias = 0. Retail and Native: 0 and not taken.
    float uiLodBias = 0.0f;
    bool uiLodBiasGlyphsOnly = true;
    bool uiTakesLodBias = false;
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
    // np-look36: the near field of the depth-weighted blur (DofPass::Run
    // `nearField`, with postDofDepthWeight). The weight kept in-focus colour
    // out of the blur in FRONT of the focus too, so a blurred crowd member
    // between the camera and the dancers kept a hard cut-out edge (retail's
    // depth-blind blur softens it). A pixel out of focus in front of the focal
    // plane takes the depth-blind blur; postDofNearSpread also spreads the near
    // object's blur over the in-focus pixels beside it. Retail and Native: off.
    // Inspection: {rnd native_look dof_near_on|_off|_rule,
    // dof_near_spread_on|_off|_rule}.
    // Both are decided by adapter class, as supersampling is (renderScale /
    // renderScaleIntegrated). The field adds a second quarter target to the
    // downsample and the two blurs (8 more reads per quarter texel in each)
    // and 4 reads in the composite inside the near field; the spread one more
    // read at every full-size pixel of the composite. postDofNearField and
    // postDofNearSpread are the values on a discrete GPU, the ...Apple pair on
    // an Apple GPU (LookAdapterClass, np-look49), the ...Integrated pair on
    // anything else (integrated, CPU, unknown, a browser not on NVIDIA).
    // Enhanced: on in all three (Apple timed on an M3, the other class on the
    // GPD's 890M, dof57; dc3 PLAN H-165). LookDofNearField /
    // LookDofNearSpread pick, and with the field off the depth of field runs
    // exactly the passes it ran before np-look36. LogLookAdapterClass prints
    // the choice once.
    bool postDofNearField = false;
    bool postDofNearFieldApple = false;
    bool postDofNearFieldIntegrated = false;
    bool postDofNearSpread = false;
    bool postDofNearSpreadApple = false;
    bool postDofNearSpreadIntegrated = false;
    // np-look51: the depth of field's edge rule (DofPass::Run `edge`, the
    // enhanced composites only). The composite mixed the resolved scene by
    // the factor of depth sample 0 alone, so a pixel an in-focus dancer half
    // covers was kept whole or replaced whole by the blurred background, and
    // the outline stepped at one sample per pixel in every close-up (dc3
    // item 51). The rule reads every depth sample; where they disagree it
    // composites per sample with the main pass's multisampled colour (each
    // sample its own factor, near value and colour, the correction added to
    // the resolved scene), so a half-covered edge pixel is half figure, half
    // blurred background (DofPass.cpp, the ..._edge composites). Three more
    // depth loads per full-size pixel of the composite, and four colour loads
    // at the pixels whose samples disagree; MSAA4xColor gains TextureBinding.
    // Where the samples agree: the composite without the rule, to the bit.
    // Decided by adapter class like the near field:
    // postDofEdge on a discrete GPU, ...Apple on an Apple GPU, ...Integrated
    // on anything else. Enhanced: on / off (kAppleDofEdge: timed on the M3,
    // over its limit at every size) / off (the 890M is not timed). Retail and
    // Native: off. Inspection: {rnd native_look dof_edge_on|_off|_rule}.
    // LookDofEdge picks.
    bool postDofEdge = false;
    bool postDofEdgeApple = false;
    bool postDofEdgeIntegrated = false;
    // np-present60 (dc3 item 60): a frame drawn SMALLER than the rectangle the
    // present fits it into (a row cap: a non-discrete GPU's 1080 rows on a
    // larger window; a thermal step) is read with a clamped Catmull-Rom cubic
    // (PresentScale::UpscaleFilter::CatmullRom5Clamp: 5 bilinear reads and 3
    // gathers a pixel) instead of one bilinear tap. Measured on slanted edges
    // stretched 1.32: the 10-90 rise 13 % narrower, no overshoot (the clamp to
    // the four nearest texels). Nothing changes at a scale of 1, a downscale,
    // or a whole-number scale (nearest). Decided by adapter class like the
    // depth of field's edge rule: presentUpscale on a discrete GPU, ...Apple
    // on an Apple GPU, ...Integrated on anything else. Enhanced: on (the
    // pass alone on an RTX 3090, 1920x1080 into 2560x1440: 0.073 ms against
    // 0.036) / off / on. The 890M timed it under its limit of 0.25 ms over
    // the one tap (np-present60b, 2026-11-22: one tap about 0.28 ms, the
    // cubic +0.004 ms into 2560x1440 and +0.012 into 2536x1426, means of
    // three runs whose spread is about 0.01; the pass is bound by its writes,
    // so the extra reads hide). Apple off: the M3 has not timed it, and an
    // Apple GPU's frame is the one a thermal step stretches. Retail and
    // Native: off (today's one tap, to the bit). Inspection:
    // {rnd native_look present_up_on|_off|_rule}. LookPresentUpscale picks.
    bool presentUpscale = false;
    bool presentUpscaleApple = false;
    bool presentUpscaleIntegrated = false;
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
    // Inspection only: no look sets this (np-look26 g, below). When on, the
    // enhanced cutout (MaterialCutCoverage == Sharpened: hair, crowd
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
    // Retail and Native: always off (no retail pipeline is fs_cutout, and the
    // tokens refuse the Native profile).
    // np-look26 f: the mode hair_edge_on selects is E (CutoutSampleMode):
    // where retail's level resolves the alpha (lod <= 1: palms, fences up
    // close) the per-sample mask alone (B, as smooth as the landed coverage);
    // where the texture is minified past retail's level 1 (hair at a
    // distance) retail's test at the pixel centre first, so every pixel
    // retail draws is drawn solid, and the per-sample mask only where the
    // centre fails (C; B alone was faithful but noisy there: four point
    // samples of sub-pixel strand alpha). Inspection:
    // {rnd native_look hair_edge_on|hair_edge_off|hair_edge_rule}, and
    // hair_edge_b|hair_edge_c|hair_edge_e|hair_edge_centre for the modes.
    // (Item 26's boost of the tested alpha by mip level, rejected: it drew
    // content authored under the cut, dclive's PeakLine floor fades.)
    // np-look26 g, why no look turns it on: a retail launch (two mip levels
    // uploaded) shows hair more broken than the landed coverage, not less --
    // the comparison that started this used the retail token in an enhanced
    // run, which sampled whole chains. Against the landed coverage, E fills
    // strand tips (oblio04's strands at dclive, 1268x713: solid share .519
    // against .489) and removes a faint dotted floor line the landed coverage
    // draws and retail does not, but it draws retail's aliased dots solid
    // (a dotted silhouette where the landed coverage is smooth), steps floor
    // stripe edges, and costs four alpha fetches per cut-out fragment. A
    // trade, so the look is unchanged; one line in SettingsFor adopts it.
    bool cutoutSampleTest = false;

    // ---- crowd contact shadows (np-look32) ------------------------------------
    // A soft dark ellipse on the ground at each 3D crowd member's feet
    // (platform/ContactShadowHook.h, gfx/ContactShadowPass.h). The original
    // draws no shadow of any kind for the crowd (WorldCrowd::Draw3DChars turns
    // self and floor shadow off in game, and the models carry none), so on
    // pale ground it floats. Enhanced: on. Inspection:
    // {rnd native_look crowd_shadow_on|crowd_shadow_off|crowd_shadow_rule}.
    bool crowdContactShadow = false;

    // ---- render-to-texture targets drawn like the frame (np-look47) ----------
    // A camera's render target drawn through a texture pass -- in DC3 a
    // RndTexRenderer's output, Break It Down's projection.tex -- was drawn at
    // its own size with one sample a texel, while the frame round it has 4x
    // MSAA at the look's render scale, so a dancer shown through it steps one
    // pixel at a time. Which targets: TexPassDrawnLikeFrame (Tex_Wgpu.cpp),
    // the one place the rule lives.
    //  texPassMsaa (step 1): the pass draws with the frame's sample count
    //    (kMSAASamples) into a multisampled colour and depth pair and resolves
    //    into the texture. The texture keeps its size and format, so a sampler
    //    or a readback of it sees what it saw, edges averaged.
    // Enhanced only; Retail and Native (rb3, rb3-xenon) keep the one-sample
    // target, pass for pass. A value per adapter class (LookTexPassMsaa):
    // texPassMsaa discrete, ...Apple, ...Integrated anything else.
    // Inspection: {rnd native_look rtt_msaa_on|rtt_msaa_off|rtt_msaa_rule}.
    bool texPassMsaa = false;
    bool texPassMsaaApple = false;
    bool texPassMsaaIntegrated = false;
    //  texPassScale (step 2): the GPU texture behind the RndTex is drawn
    //    larger, by the OUTPUT's rows (GpuDevice::OutputHeight, the rows the
    //    frame leaves the renderer at, whatever its render scale) over
    //    texPassBaseRows: a 1024 target is 1024 up to 360 rows, 1536 at 540
    //    rows, 2048 (texPassMaxSide, the longer side's cap) from 720 rows up;
    //    1080 at render scale 2 gets what 1080 at scale 1 gets. That is about
    //    two texels a screen pixel for a figure that fills the frame's height
    //    at 720 rows: with one (base rows 720) the target is resampled
    //    bilinearly at about 1 to 1 and the figure is softer than the same
    //    character in the main frame (dc3 PLAN H-175: mean |Laplacian| inside
    //    the figure 15.4 / 22.0 / 26.0 at 1024 / 1536 / 2048). texPassBaseRows
    //    is the one constant of the rule: 480 gives 1536 at 720 rows, the
    //    fallback if thin lines are seen to crawl in play. Both numbers are
    //    per adapter class (np-look49), as the switch is: texPassMaxSide /
    //    texPassBaseRows discrete, ...Apple, ...Integrated anything else
    //    (LookTexPassMaxSide / LookTexPassBaseRows); the numbers above are the
    //    discrete class's. Apple: cap 1536, base rows 720 (timed on an M3:
    //    1024 up to 720 rows, 1536 from 1080 rows up). Other non-discrete
    //    (np-rtt-rule57): cap 1536, base rows 720, Apple's pair (timed on the
    //    890M: a 1024 projection is 1024, 4 samples, up to 720 rows and 1536
    //    from 1080 rows up). Width()/
    //    Height() of the RndTex do not change, and every reader of them sees
    //    the texture's own size: viewports stay in its pixels (FrameViewport
    //    scales them to the attachment), a CPU readback is box-filtered back
    //    to its own size, a CPU write puts the target at its own size and
    //    holds it there. The size changes only where a pass that clears the
    //    target begins (WgpuRnd::BeginTexturePass), never where it is sampled.
    //    Inspection: {rnd native_look rtt_scale_on|rtt_scale_off|rtt_scale_rule};
    //    the two numbers: rtt_cap_1024|1536|2048|rule, rtt_rows_360|480|720|rule
    //    (SetLookTexPassSizeForInspection; a number token sets that number for
    //    all three classes, its rule returns each to its own). Per adapter
    //    class as texPassMsaa.
    bool texPassScale = false;
    bool texPassScaleApple = false;
    bool texPassScaleIntegrated = false;
    int texPassMaxSide = 2048;
    int texPassMaxSideApple = 2048;
    int texPassMaxSideIntegrated = 2048;
    int texPassBaseRows = 360;
    int texPassBaseRowsApple = 360;
    int texPassBaseRowsIntegrated = 360;
};

// The cut-out modes the scene block carries (SceneUniforms::cutoutSampleTest,
// a float holding one of these; standard_wgsl.inc cutoutSampleMask). All are
// inspection tokens, kept so one held frame can be measured under each;
// kCutoutModeDefault is the one hair_edge_on (and a look that sets
// cutoutSampleTest, none today) selects.
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
constexpr int kCutoutModeDefault = kCutoutModeE;  // hair_edge_on's mode (np-look26 f; was C in e)

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
// np-look34: whether uiClass draws read the text bias at all under `look`
// (PipelineKey::uiBias): look.uiTakesLodBias. False for retail and Native,
// whose text pipelines then sample exactly as before.
bool LookTakesUiLodBias(const LookSettings &look);
// The two values a pass's scene block carries: the glyph-page bias, and the
// bias of the other uiClass art (0 while uiLodBiasGlyphsOnly).
float UiGlyphLodBias(const LookSettings &look);
float UiArtLodBias(const LookSettings &look);
// {rnd native_look text_bias_off|text_bias_half|text_bias_on|text_bias_rule}:
// uiLodBias 0 / -0.5 / -1, or the profile's own value (useRule). A uniform:
// the next frame, nothing compiles. False under Native.
bool SetLookUiLodBiasForInspection(bool useRule, float bias);
// {rnd native_look text_levels_2|text_levels_all|text_levels_rule}: the text
// and HUD samplers' level clamp (uiMipLevels 2, 0 = no clamp, or the
// profile's; levels < 0). One sampler and bind-group cache miss per material,
// as the anisotropy tokens. False under Native.
bool SetLookUiMipLevelsForInspection(int levels);
// {rnd native_look text_bias_glyphs|text_bias_all_ui|text_bias_scope_rule}:
// uiLodBiasGlyphsOnly true / false / the profile's (mode 1 / 0 / -1). A
// uniform. False under Native.
bool SetLookUiLodBiasScopeForInspection(int mode);
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
// {rnd native_look dof_near_on|_off|_rule} / {dof_near_spread_on|_off|_rule}:
// postDofNearField / postDofNearSpread on, off or the profile's (1 / 0 / -1).
// The first switch of the near field on in a run that did not start with it
// compiles four pipelines; the spread is a uniform. False under Native.
// Each token writes all three adapter classes' values: on and off force it on
// any GPU, and the rule returns each class to the profile's.
bool SetLookDofNearForInspection(int mode, bool spread = false);
// The near field and its spread for this adapter class: postDofNearField /
// postDofNearSpread on a discrete GPU, the ...Apple pair on an Apple GPU, the
// ...Integrated pair on anything else. Callers pass GpuDevice::LookClass().
bool LookDofNearField(const LookSettings &look, LookAdapterClass c);
bool LookDofNearSpread(const LookSettings &look, LookAdapterClass c);
// {rnd native_look dof_edge_on|_off|_rule} (np-look51): postDofEdge on, off or
// the profile's (1 / 0 / -1), all three adapter classes, as the near field's
// tokens do. A switch on compiles the edge rule's composite once. False under
// Native, changing nothing.
bool SetLookDofEdgeForInspection(int mode);
// {rnd native_look present_up_on|_off|_rule} (np-present60): presentUpscale
// on, off or the profile's (1 / 0 / -1), all three classes' fields as the
// edge rule's tokens do. False under Native, changing nothing.
bool SetLookPresentUpscaleForInspection(int mode);
// The present's upscale for this adapter class: presentUpscale on a discrete
// GPU, ...Apple on an Apple GPU, ...Integrated on anything else. Callers pass
// GpuDevice::LookClass().
bool LookPresentUpscale(const LookSettings &look, LookAdapterClass c);
// The edge rule for this adapter class: postDofEdge on a discrete GPU,
// ...Apple on an Apple GPU, ...Integrated on anything else. Callers pass
// GpuDevice::LookClass().
bool LookDofEdge(const LookSettings &look, LookAdapterClass c);
// Prints, once per process, the adapter class the device reported, the facts
// it was decided from and what the five class-decided settings came to on it
// ("[look] adapter class ..."). Nothing under the Native profile, which has no
// class-decided setting here. The renderer calls it when its device is up.
void LogLookAdapterClass(const LookAdapterFacts &adapter);
// The same line, every call (LogLookAdapterClass's body; tests read it).
void PrintLookAdapterClass(const LookAdapterFacts &adapter);
// The adapter LogLookAdapterClass was last given (default facts before any).
// Every call records it, logged or not; the class token's line names it.
LookAdapterFacts LoggedLookAdapter();
// {rnd native_look adapter_apple|adapter_other|adapter_discrete|adapter_rule}
// (np-look49): force the class the look's per-adapter rules use
// (LookAdapterClassInEffect, so every caller of GpuDevice::LookClass()), or
// (useAdapter) return to the adapter's own class. Look rules only: render
// scale, the output row cap, frame sizing and GpuDevice::IsDiscreteGpu()
// never read it. Each rule takes it as it takes its own token: a texture pass
// target at its next clearing pass, the depth of field on the next frame (the
// near field's first switch on compiles four pipelines). Kept across a
// profile switch (it stands for the adapter, not the look); a
// ScopedLookOverride starts with none and restores it. Prints that the class
// is forced, which class the adapter really is (LoggedLookAdapter) and the
// four outcomes; useAdapter prints the ordinary class line. False under the
// Native profile, changing nothing.
bool SetLookAdapterClassForInspection(bool useAdapter,
                                      LookAdapterClass forced = LookAdapterClass::OtherNonDiscrete);
// The class the look's rules use for this adapter: the forced class while
// one is, else ClassifyLookAdapter(adapter). GpuDevice::LookClass() is this.
LookAdapterClass LookAdapterClassInEffect(const LookAdapterFacts &adapter);
// {rnd native_look hair_edge_on|hair_edge_off|hair_edge_rule}: cutoutSampleTest
// on (mode E, kCutoutModeDefault), off, or the profile's own value, which is
// off in every look (mode 1 / 0 / -1), so one frozen frame is measured both
// ways; each clears a mode token. A uniform:
// takes effect on the next frame and compiles nothing. False under the Native
// profile.
bool SetLookCutoutSampleTestForInspection(int mode);
// crowd_shadow_on|off|rule: LookSettings::crowdContactShadow on, off, or the
// profile's own value (mode < 0). False under Native.
bool SetLookCrowdShadowForInspection(int mode);
// np-look47: texPassMsaa (step 1) on, off or the profile's (1 / 0 / -1),
// all three adapter classes. False under Native.
bool SetLookTexPassMsaaForInspection(int mode);
// Step 1 for this adapter class.
bool LookTexPassMsaa(const LookSettings &look, LookAdapterClass c);
// np-look47 step 2: texPassScale on, off or the profile's (1 / 0 / -1), all
// three adapter classes. False under Native.
bool SetLookTexPassScaleForInspection(int mode);
// The two numbers of step 2's size rule (TexPassGpuSize): the cap
// (texPassMaxSide...) and the base rows (texPassBaseRows...). Each argument
// > 0 sets that number for all three adapter classes (so a token means the
// same on any GPU), 0 leaves it alone, < 0 returns each class's to the
// profile's. Does not turn texPassScale on or off; a target takes the new size
// at its next clearing pass, as for the scale token. Prints each number for
// the three classes. False under Native, changing nothing.
bool SetLookTexPassSizeForInspection(int maxSide, int baseRows);
bool LookTexPassScale(const LookSettings &look, LookAdapterClass c);
// Step 2's two numbers for this adapter class.
int LookTexPassMaxSide(const LookSettings &look, LookAdapterClass c);
int LookTexPassBaseRows(const LookSettings &look, LookAdapterClass c);
// The GPU size of a texW x texH target under texPassScale for an output of
// outputRows rows on adapter class c (GpuDevice::LookClass()): each side times
// max(1, outputRows / the class's base rows), the longer side at most the
// class's cap (never below the texture's own).
void TexPassGpuSize(const LookSettings &look, LookAdapterClass c, int texW, int texH, int outputRows, int &gpuW,
                    int &gpuH);
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
// np-gpuprof2: the predicted frame-pass resolve (WgpuRnd, DC3's shape). On by
// default (pixel-identical; MILO_RESOLVE_PREDICT=0 starts it off); -1 the
// default, 0 off, 1 on: {rnd native_look resolve_predict_off|_on|_rule}.
void SetResolvePredictForInspection(int mode);
bool ResolvePredictActive();
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
    int mPrevAdapterClassInspection;
};

}  // namespace milo
