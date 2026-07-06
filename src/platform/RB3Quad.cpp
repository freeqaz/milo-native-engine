// RB3 2D quad pipeline (DrawRect + the outfit two-colour compose lerp) —
// extracted VERBATIM from BandRnd:: methods in platform/Rnd_Wgpu_RB3.cpp
// (W1.4.S3). RB3-only TU (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3). Pure MOVE:
// the largest of the three W1.4 blocks but the most self-contained (one
// contiguous span) — copied token-for-token. See RB3Quad.h / W1.4 PLAN for
// why this is a NEW RB3-only TU, not a convergence onto gfx/DrawRect2D.
//
// Moved defs: kRB3QuadShaderSource include, RB3RectUB / RB3RectVertex CPU
// mirror structs, EnsureQuadPipeline (incl. its internal
// kRB3ComposeShaderSource include + all mCompose*/mQuadPost* infra), the
// static RB3QuadPipeKey() helper (moves with its only callers, stays
// static), the non-static gRB3OutfitComposeActive global (OutfitConfig.cpp
// externs it), and DrawRect. DrawRect calls the now-cross-TU
// RB3RttDisabled() (Rnd_Wgpu_RB3.cpp, W1.4.S3 linkage MOVE) and references
// kRB3PostProcShaderSource / PostProcUniforms (RB3PostProc.h, W1.4.S2).

#include "platform/Rnd_Wgpu_RB3.h"
#include "platform/RB3Quad.h"
#include "platform/RB3PostProc.h"        // kRB3PostProcShaderSource / PostProcUniforms
#include "platform/RB3MaterialBinder.h"  // GetRB3TexView / UploadRndTexIfNeeded

#include "rndobj/Cam.h"
#include "rndobj/Mat.h"
#include "rndobj/Tex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ===========================================================================
// Shared 2D quad pipeline infra (§3 of the RTT engine plan).
//
// ONE WGSL module holds every quad entry point so Stage 2's postproc composite
// reuses the same shader handle:
//   - vs_rect            : explicit 6-vertex NDC quad (positions mapped CPU-side
//                          in DrawRect; passes uv + per-vertex color through).
//   - fs_rect            : textured/color-modulated rect — tex*mod*vtxColor, with
//                          colorMod==kColorModAlphaUnpackModulate(2) sampling the
//                          diffuse's ALPHA as a grayscale mask (v1 approx).
//   - fs_rect_notex      : mod*vtxColor (base layer has a null diffuse).
//   - vs_fullscreen      : Stage-2 fullscreen-triangle (no vbuf) — added later.
//   - fs_postproc        : Stage-2 grade fragment — added later.
//
// RectUB (32B, group 0 binding 2): mod (vec4) + flags (uvec4; only .x =
// colorMod is read). mod = mat->GetColor() * paramColor — the KEY divergence
// from dc3's DrawRect2D (which ignores matColor and would yield NO tint here,
// because Compose passes a white param color and sets the real tint via
// sMat->SetColor()).
// ===========================================================================
static const char* kRB3QuadShaderSource =
#include "gfx/Shaders/rb3_quad.wgsl.inc"
;

// CPU mirror of the 32-byte RectUB (matches the WGSL struct std140 layout:
// vec4 + uvec4 = 16 + 16 = 32 bytes).
struct RB3RectUB {
    float mod[4];
    uint32_t flags[4];
};

// CPU mirror of the per-vertex 2D quad layout (matches vs_rect inputs).
struct RB3RectVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

void BandRnd::EnsureQuadPipeline() {
    if (mQuadReady) return;
    auto& dev = mGpu.Device();

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kRB3QuadShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    mQuadShader = dev.CreateShaderModule(&smDesc);

    // Rect bind-group layout: tex@0, sampler@1, RectUB@2.
    wgpu::BindGroupLayoutEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[2].buffer.minBindingSize = sizeof(RB3RectUB);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    mQuadRectBGL = dev.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mQuadRectBGL;
    mQuadRectPL = dev.CreatePipelineLayout(&plDesc);

    wgpu::BufferDescriptor vbDesc{};
    vbDesc.size = 6 * sizeof(RB3RectVertex);
    vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    mQuadVertexBuffer = dev.CreateBuffer(&vbDesc);

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = sizeof(RB3RectUB);
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mRectUB = dev.CreateBuffer(&ubDesc);

    // --- Outfit two-color composite: interp-lerp pass (diff + interp) ---
    // Separate module: it needs a 2-texture @group(0) layout (diff@0, interp@1,
    // samp@2, ComposeUB@3) which conflicts with fs_rect's bindings, so it cannot
    // share mQuadShader. vs_compose matches the same RB3RectVertex layout so the
    // pass reuses mQuadVertexBuffer (already holding the full-screen quad).
    static const char* kRB3ComposeShaderSource =
#include "gfx/Shaders/rb3_compose.wgsl.inc"
    ;
    wgpu::ShaderSourceWGSL cWgsl{};
    cWgsl.code = kRB3ComposeShaderSource;
    wgpu::ShaderModuleDescriptor cSmDesc{};
    cSmDesc.nextInChain = &cWgsl;
    mComposeShader = dev.CreateShaderModule(&cSmDesc);

    wgpu::BindGroupLayoutEntry cEntries[4] = {};
    cEntries[0].binding = 0;
    cEntries[0].visibility = wgpu::ShaderStage::Fragment;
    cEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    cEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    cEntries[1].binding = 1;
    cEntries[1].visibility = wgpu::ShaderStage::Fragment;
    cEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    cEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    cEntries[2].binding = 2;
    cEntries[2].visibility = wgpu::ShaderStage::Fragment;
    cEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
    cEntries[3].binding = 3;
    cEntries[3].visibility = wgpu::ShaderStage::Fragment;
    cEntries[3].buffer.type = wgpu::BufferBindingType::Uniform;
    cEntries[3].buffer.minBindingSize = 16;
    wgpu::BindGroupLayoutDescriptor cBglDesc{};
    cBglDesc.entryCount = 4;
    cBglDesc.entries = cEntries;
    mComposeBGL = dev.CreateBindGroupLayout(&cBglDesc);

    wgpu::PipelineLayoutDescriptor cPlDesc{};
    cPlDesc.bindGroupLayoutCount = 1;
    cPlDesc.bindGroupLayouts = &mComposeBGL;
    mComposePL = dev.CreatePipelineLayout(&cPlDesc);

    wgpu::BufferDescriptor cUbDesc{};
    cUbDesc.size = 16;
    cUbDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mComposeUB = dev.CreateBuffer(&cUbDesc);

    // --- Stage 2: postproc grade module + bind-group layout + UB ---
    wgpu::ShaderSourceWGSL ppWgsl{};
    ppWgsl.code = kRB3PostProcShaderSource;
    wgpu::ShaderModuleDescriptor ppSmDesc{};
    ppSmDesc.nextInChain = &ppWgsl;
    mQuadPostShader = dev.CreateShaderModule(&ppSmDesc);

    // sceneTex@0, sampler@1, PostProcUB@2 (160B, minBindingSize=160),
    // bloomTex@3, noiseTex@4 (V2 tiled grain).
    wgpu::BindGroupLayoutEntry ppEntries[5] = {};
    ppEntries[0].binding = 0;
    ppEntries[0].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    ppEntries[1].binding = 1;
    ppEntries[1].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    ppEntries[2].binding = 2;
    ppEntries[2].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    ppEntries[2].buffer.minBindingSize = sizeof(PostProcUniforms);  // 160
    ppEntries[3].binding = 3;
    ppEntries[3].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[3].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    ppEntries[4].binding = 4;
    ppEntries[4].visibility = wgpu::ShaderStage::Fragment;
    ppEntries[4].texture.sampleType = wgpu::TextureSampleType::Float;
    ppEntries[4].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    wgpu::BindGroupLayoutDescriptor ppBglDesc{};
    ppBglDesc.entryCount = 5;
    ppBglDesc.entries = ppEntries;
    mQuadPostBGL = dev.CreateBindGroupLayout(&ppBglDesc);

    wgpu::PipelineLayoutDescriptor ppPlDesc{};
    ppPlDesc.bindGroupLayoutCount = 1;
    ppPlDesc.bindGroupLayouts = &mQuadPostBGL;
    mQuadPostPL = dev.CreatePipelineLayout(&ppPlDesc);

    wgpu::BufferDescriptor ppUbDesc{};
    ppUbDesc.size = sizeof(PostProcUniforms);
    ppUbDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mPostProcUB = dev.CreateBuffer(&ppUbDesc);

    mQuadReady = true;
}

// Get-or-create a rect RenderPipeline keyed on (format, blend, hasDepth, isPost,
// hasTex). The composite (Stage 2) and repeated DrawRect calls share the cache
// so we never CreateRenderPipeline per invocation. `notex` selects the
// fs_rect_notex entry (base layer, null diffuse).
static uint64_t RB3QuadPipeKey(wgpu::TextureFormat fmt, WgpuBlend blend,
                               bool hasDepth, bool isPost, bool notex) {
    return ((uint64_t)(uint32_t)fmt << 8) | ((uint64_t)(uint32_t)blend << 3) |
           ((uint64_t)(hasDepth ? 1 : 0) << 2) | ((uint64_t)(isPost ? 1 : 0) << 1) |
           (uint64_t)(notex ? 1 : 0);
}

// RB3 outfit two-color composite scope flag. Set by OutfitConfig::MatSwap::
// Compose (rb3 side, HX_NATIVE-guarded) around the four-pass DrawRect sequence
// that paints an outfit's *_diffuse_output render target (eyes, skin, hair,
// clothing, instruments). Lets DrawRect below identify the modulate layers of
// that composite so they can be combined with a DEST-MULTIPLY blend (the layers
// are authored to product-combine on Wii/360 via ColorModFlags TEV/shader
// modes; native's DrawRect otherwise keeps REPLACE and the RT collapses to
// "last layer wins" -> a near-white flat texture -> glowing white eyeballs).
// Defined here so the engine always links; RB3-only TU so DC3 is unaffected.
bool gRB3OutfitComposeActive = false;

void BandRnd::DrawRect(const Hmx::Rect& rect, const Hmx::Color& paramColor,
                       RndMat* mat, const Hmx::Color* topRight,
                       const Hmx::Color* botLeft) {
    if (!mGpuReady || !mInPass) return;

    // CRITICAL RTT begin-hook (mirrors DrawMesh ~:1188): Compose calls DrawRect
    // BEFORE any DrawMesh, so the lazy begin-redirect that DrawMesh normally
    // performs has not run yet. If the current cam targets an RTT tex we haven't
    // redirected to, open the RT pass now — otherwise the outfit tint paints the
    // MAIN framebuffer instead of the RTT diffuse texture.
    if (!RB3RttDisabled() && RndCam::sCurrent) {
        RndTex* tt = RndCam::sCurrent->TargetTex();
        if (tt && tt != mRtActiveTex) BeginDrawTarget(tt);
    }
    if (!mInPass) return;   // BeginDrawTarget bailed and left no open pass

    EnsureQuadPipeline();
    if (!mQuadShader || !mQuadVertexBuffer || !mRectUB) return;

    // Rect is absolute Rnd-PIXEL space (e.g. 0..Width x 0..Height). Map to NDC
    // via TheRnd->Width()/Height() — NOT the GPU framebuffer size.
    float w = (float)Width();
    float h = (float)Height();
    if (w <= 0.0f || h <= 0.0f) return;

    float x0 = rect.x / w * 2.0f - 1.0f;
    float y0 = 1.0f - rect.y / h * 2.0f;
    float x1 = (rect.x + rect.w) / w * 2.0f - 1.0f;
    float y1 = 1.0f - (rect.y + rect.h) / h * 2.0f;

    // Per-vertex color carries the optional top-right / bottom-left gradient
    // (Compose always passes white + null gradients; the real tint is the UB
    // mod). cTL = paramColor; cBR averaged.
    float cTL[4] = { paramColor.red, paramColor.green, paramColor.blue, paramColor.alpha };
    float cTR[4], cBL[4], cBR[4];
    if (topRight) { cTR[0]=topRight->red; cTR[1]=topRight->green; cTR[2]=topRight->blue; cTR[3]=topRight->alpha; }
    else          { std::memcpy(cTR, cTL, sizeof(cTL)); }
    if (botLeft)  { cBL[0]=botLeft->red;  cBL[1]=botLeft->green;  cBL[2]=botLeft->blue;  cBL[3]=botLeft->alpha; }
    else          { std::memcpy(cBL, cTL, sizeof(cTL)); }
    for (int i = 0; i < 4; i++) cBR[i] = (cTR[i] + cBL[i]) * 0.5f;

    RB3RectVertex verts[6] = {
        {{x0, y0}, {0, 0}, {cTL[0], cTL[1], cTL[2], cTL[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x1, y0}, {1, 0}, {cTR[0], cTR[1], cTR[2], cTR[3]}},
        {{x0, y1}, {0, 1}, {cBL[0], cBL[1], cBL[2], cBL[3]}},
        {{x1, y1}, {1, 1}, {cBR[0], cBR[1], cBR[2], cBR[3]}},
    };
    mGpu.Queue().WriteBuffer(mQuadVertexBuffer, 0, verts, sizeof(verts));

    // Modulation = mat->GetColor() * paramColor. THE key DC3 divergence:
    // Compose passes a white paramColor and sets the real tint via
    // sMat->SetColor(), so the modulation MUST fold mat->GetColor().
    int colorMod = 0;
    Hmx::Color matCol(1.0f, 1.0f, 1.0f, 1.0f);
    if (mat) {
        matCol = mat->GetColor();
        colorMod = (int)mat->mColorModFlags;
    }
    RB3RectUB ub{};
    ub.mod[0] = matCol.red   * paramColor.red;
    ub.mod[1] = matCol.green * paramColor.green;
    ub.mod[2] = matCol.blue  * paramColor.blue;
    ub.mod[3] = matCol.alpha * paramColor.alpha;
    ub.flags[0] = (uint32_t)colorMod;
    mGpu.Queue().WriteBuffer(mRectUB, 0, &ub, sizeof(ub));

    // Diffuse: GetRB3TexView(mat->GetDiffuseTex()), uploading on first use, with
    // mWhiteView fallback. The base layer has a null diffuse → fs_rect_notex.
    bool hasTex = false;
    wgpu::TextureView texView;
    RndTex* diffuse = mat ? mat->GetDiffuseTex() : nullptr;
    if (diffuse) {
        texView = GetRB3TexView(diffuse);
        if (!texView) texView = UploadRndTexIfNeeded(mGpu, diffuse);
        if (texView) hasTex = true;
    }
    // RB3_SCREENMASK_FIX (default-ON): when the material's diffuse is a RENDER
    // TARGET that was NEVER painted (no GPU view), SKIP this quad entirely
    // instead of blitting the 1x1 mWhiteView. This is the festival
    // `crowd_mass.*` screenmask case: `crowd_mass.tex` is an RT fed ONLY by a
    // TexMovie Bink movie, which has no in-world decoder on native -> the RT is
    // never painted -> the old `texView = mWhiteView` fallback blitted opaque
    // white over the whole screen, blanking the festival mass-crowd shots.
    // Skipping reveals the band + venue/world geometry behind it (far closer to
    // retail than full-screen white).
    //
    // DISCRIMINATOR (must skip ONLY the dead movie RT):
    //   - diffuse->IsRenderTarget()  -> it is a `kRendered`-type RT
    //   - !hasTex                    -> NO painted view resolved
    // A PAINTED RT (sky-dome `clouds_rnd.tex`) goes through BeginDrawTarget,
    // which sets `uploaded=true` + a valid `view`, so GetRB3TexView returns it,
    // hasTex==true, and it is NOT skipped. A null / non-RT diffuse (solid-color
    // UI rects that legitimately want the white fallback) is not an RT, so it is
    // unaffected and still gets mWhiteView as before. Opt-out
    // RB3_SCREENMASK_FALLBACK_OFF=1 restores the original white blit (proves the
    // gate). RB3-only TU (DC3 compiles Rnd_Wgpu.cpp, never this file).
    if (!hasTex && diffuse && diffuse->IsRenderTarget()) {
        static const bool kScreenmaskFallbackOff =
            getenv("RB3_SCREENMASK_FALLBACK_OFF") != nullptr;
        if (!kScreenmaskFallbackOff) {
            if (getenv("RB3_SCREENMASK_DBG"))
                fprintf(stderr, "[dbg] DrawRect skip unpainted-RT diffuse '%s'\n",
                        diffuse->Name() ? diffuse->Name() : "?");
            return;   // skip the quad — reveal what is behind the dead movie RT
        }
    }
    if (!hasTex) texView = mWhiteView;

    // Blend via the shared MapBlend; target format/depth per the ACTIVE pass.
    WgpuBlend blend = WgpuBlend::Src;
    if (mat) {
        int b = (int)mat->GetBlend();
        if (b >= 0 && b <= 10) blend = (WgpuBlend)b;
    }
    bool rtPass = (mRtActiveTex != nullptr);
    wgpu::TextureFormat fmt = rtPass ? mRtFmt : mTargetFmt;   // NEVER hardcode RGBA8
    bool hasDepth = !rtPass;   // RT pass: no depth; main pass: depth-disabled D24S8

    // RB3 outfit two-color composite (default-ON, opt-out RB3_COMPOSE_MULT_OFF=1
    // restores the pre-fix pure-REPLACE behavior for A/B). The authored recolor
    // is  out.rgb = diff.rgb * lerp(color1, color2, w) , with w = the interp
    // map's ALPHA (its RGB is a white carrier) — NOT a whole-RT replace (last
    // layer wins -> near-white flat texture / glowing eyes) nor a pure product
    // (color1*color2*diff -> black silhouettes). Compose issues the recolor as a
    // layered DrawRect sequence into a *_diffuse_output RT while
    // gRB3OutfitComposeActive is set; we collapse those layers:
    //   colorMod 0 (base fill, no tex): record color1, REPLACE fill (fallback for
    //       areas the diff doesn't cover + the tattoo/logo patches drawn after).
    //   colorMod 3, first w/ tex (DIFF, tint=color2): record diff view + color2,
    //       draw diff.rgb*color1 REPLACE (correct already for skin, where color2
    //       is white and w~0 -> diff*color1).
    //   colorMod 3, second (INTERP, white RGB, alpha=w): a 2-texture pass that
    //       alpha-over-blends diff*color2 weighted by w over the diff*color1 dest
    //       -> diff*lerp(color1,color2,w). Handled inline below (early return).
    //   colorMod 2 (mask, alpha=coverage): DEST-MULTIPLY (coverage modulation).
    // Scoped by gRB3OutfitComposeActive + mRtActiveTex so postproc/vignette
    // DrawRects (which also carry colorMod) are untouched. RB3-only TU.
    static const bool kComposeMultOff = getenv("RB3_COMPOSE_MULT_OFF") != nullptr;
    bool composeActive = !kComposeMultOff && gRB3OutfitComposeActive && rtPass;
    if (composeActive) {
        if (colorMod == 0) {
            // Base fill: record color1, reset per-composite state. REPLACE fill.
            mComposeColor1 = matCol;
            mComposeHaveDiff = false;
        } else if (colorMod == 3 && !mComposeHaveDiff && hasTex) {
            // DIFF layer: capture diff view + tint (color2), then draw
            // diff.rgb * color1 (REPLACE). Override the modulation to color1.
            mComposeColor2 = matCol;
            mComposeDiffView = texView;
            mComposeHaveDiff = true;
            ub.mod[0] = mComposeColor1.red;
            ub.mod[1] = mComposeColor1.green;
            ub.mod[2] = mComposeColor1.blue;
            ub.mod[3] = 1.0f;
            mGpu.Queue().WriteBuffer(mRectUB, 0, &ub, sizeof(ub));
            blend = WgpuBlend::Src;
        } else if (colorMod == 3 && mComposeHaveDiff && hasTex && mComposeDiffView) {
            // INTERP layer: 2-texture lerp pass (diff = captured, interp = current
            // texView). Reuses mQuadVertexBuffer (already holding this call's
            // full-screen quad). Alpha-over the diff*color1 destination.
            EnsureQuadPipeline();
            float cub[4] = { mComposeColor2.red, mComposeColor2.green,
                             mComposeColor2.blue, 1.0f };
            mGpu.Queue().WriteBuffer(mComposeUB, 0, cub, sizeof(cub));

            uint64_t ckey = RB3QuadPipeKey(fmt, WgpuBlend::SrcAlpha, hasDepth,
                                           /*isPost*/ false, /*notex*/ false);
            wgpu::RenderPipeline cpipe;
            auto cit = mComposePipelines.find(ckey);
            if (cit != mComposePipelines.end()) {
                cpipe = cit->second;
            } else {
                wgpu::BlendState cbs = mPipelines.MapBlend(WgpuBlend::SrcAlpha);
                wgpu::ColorTargetState cct{};
                cct.format = fmt;
                cct.blend = &cbs;
                cct.writeMask = wgpu::ColorWriteMask::All;
                wgpu::FragmentState cfrag{};
                cfrag.module = mComposeShader;
                cfrag.entryPoint = "fs_compose_interp";
                cfrag.targetCount = 1;
                cfrag.targets = &cct;
                wgpu::VertexAttribute cattrs[3] = {};
                cattrs[0].format = wgpu::VertexFormat::Float32x2; cattrs[0].offset = 0;  cattrs[0].shaderLocation = 0;
                cattrs[1].format = wgpu::VertexFormat::Float32x2; cattrs[1].offset = 8;  cattrs[1].shaderLocation = 1;
                cattrs[2].format = wgpu::VertexFormat::Float32x4; cattrs[2].offset = 16; cattrs[2].shaderLocation = 2;
                wgpu::VertexBufferLayout cvbl{};
                cvbl.arrayStride = sizeof(RB3RectVertex);
                cvbl.stepMode = wgpu::VertexStepMode::Vertex;
                cvbl.attributeCount = 3;
                cvbl.attributes = cattrs;
                wgpu::DepthStencilState cds{};
                cds.format = wgpu::TextureFormat::Depth24PlusStencil8;
                cds.depthWriteEnabled = wgpu::OptionalBool::False;
                cds.depthCompare = wgpu::CompareFunction::Always;
                wgpu::RenderPipelineDescriptor cpd{};
                cpd.layout = mComposePL;
                cpd.vertex.module = mComposeShader;
                cpd.vertex.entryPoint = "vs_compose";
                cpd.vertex.bufferCount = 1;
                cpd.vertex.buffers = &cvbl;
                cpd.fragment = &cfrag;
                cpd.depthStencil = hasDepth ? &cds : nullptr;
                cpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
                cpd.multisample.count = 1;
                cpipe = mGpu.Device().CreateRenderPipeline(&cpd);
                mComposePipelines[ckey] = cpipe;
            }
            if (cpipe) {
                wgpu::BindGroupEntry cbge[4] = {};
                cbge[0].binding = 0; cbge[0].textureView = mComposeDiffView;
                cbge[1].binding = 1; cbge[1].textureView = texView;
                cbge[2].binding = 2; cbge[2].sampler = mSampler;
                cbge[3].binding = 3; cbge[3].buffer = mComposeUB; cbge[3].offset = 0; cbge[3].size = 16;
                wgpu::BindGroupDescriptor cbgd{};
                cbgd.layout = mComposeBGL;
                cbgd.entryCount = 4;
                cbgd.entries = cbge;
                wgpu::BindGroup cbg = mGpu.Device().CreateBindGroup(&cbgd);
                mPass.SetPipeline(cpipe);
                mPass.SetBindGroup(0, cbg, 0, nullptr);
                mPass.SetVertexBuffer(0, mQuadVertexBuffer, 0, sizeof(verts));
                mPass.Draw(6);
                mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);
            }
            return;
        } else if (colorMod == 2) {
            // Mask layer: coverage modulation -> dest-multiply.
            blend = WgpuBlend::Multiply;
        }
    }

    uint64_t pkey = RB3QuadPipeKey(fmt, blend, hasDepth, /*isPost*/ false, /*notex*/ !hasTex);
    wgpu::RenderPipeline pipe;
    {
        auto it = mQuadPipelines.find(pkey);
        if (it != mQuadPipelines.end()) {
            pipe = it->second;
        } else {
            wgpu::BlendState bs = mPipelines.MapBlend(blend);
            wgpu::ColorTargetState ct{};
            ct.format = fmt;
            ct.blend = &bs;
            ct.writeMask = wgpu::ColorWriteMask::All;

            wgpu::FragmentState frag{};
            frag.module = mQuadShader;
            frag.entryPoint = hasTex ? "fs_rect" : "fs_rect_notex";
            frag.targetCount = 1;
            frag.targets = &ct;

            wgpu::VertexAttribute attrs[3] = {};
            attrs[0].format = wgpu::VertexFormat::Float32x2; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
            attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 8;  attrs[1].shaderLocation = 1;
            attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 16; attrs[2].shaderLocation = 2;
            wgpu::VertexBufferLayout vbl{};
            vbl.arrayStride = sizeof(RB3RectVertex);
            vbl.stepMode = wgpu::VertexStepMode::Vertex;
            vbl.attributeCount = 3;
            vbl.attributes = attrs;

            // Main pass attaches a Depth24PlusStencil8 buffer; the pipeline must
            // declare a matching depth-stencil state. Disable depth entirely
            // (compare Always, write false) so the 2D quad always paints.
            wgpu::DepthStencilState ds{};
            ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
            ds.depthWriteEnabled = wgpu::OptionalBool::False;
            ds.depthCompare = wgpu::CompareFunction::Always;

            wgpu::RenderPipelineDescriptor pd{};
            pd.layout = mQuadRectPL;
            pd.vertex.module = mQuadShader;
            pd.vertex.entryPoint = "vs_rect";
            pd.vertex.bufferCount = 1;
            pd.vertex.buffers = &vbl;
            pd.fragment = &frag;
            pd.depthStencil = hasDepth ? &ds : nullptr;
            pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pd.multisample.count = 1;   // rb3 backend is single-sampled

            pipe = mGpu.Device().CreateRenderPipeline(&pd);
            mQuadPipelines[pkey] = pipe;
        }
    }
    if (!pipe) return;

    wgpu::BindGroupEntry bge[3] = {};
    bge[0].binding = 0; bge[0].textureView = texView;
    bge[1].binding = 1; bge[1].sampler = mSampler;
    bge[2].binding = 2; bge[2].buffer = mRectUB; bge[2].offset = 0; bge[2].size = sizeof(RB3RectUB);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mQuadRectBGL;
    bgd.entryCount = 3;
    bgd.entries = bge;
    wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);

    mPass.SetPipeline(pipe);
    mPass.SetBindGroup(0, bg, 0, nullptr);
    mPass.SetVertexBuffer(0, mQuadVertexBuffer, 0, sizeof(verts));
    mPass.Draw(6);

    // CRITICAL: restore the SCENE bind group at group 0 — DrawRect rebinds
    // group 0 to its own 2D layout, and the next DrawMesh aborts in Dawn
    // (bind-group/layout mismatch) unless we put the scene group back.
    mPass.SetBindGroup(0, mSceneBindGroup, 0, nullptr);

    // One-shot RB3_DRAWRECT_DBG: report the rect, modulation color, diffuse
    // name, colorMod, and whether the RT redirect was active (verification
    // fallback when the live outfit-compose path is hard to frame).
    if (getenv("RB3_DRAWRECT_DBG")) {
        // Cap per kind (main-pass vs RTT) so a per-frame full-screen background
        // rect (e.g. movie.tex, rtActive=0) can't drown out the rarer outfit
        // RTT-compose rects (rtActive=1, the path this stage targets).
        static int sShotsMain = 0, sShotsRtt = 0;
        int& cnt = rtPass ? sShotsRtt : sShotsMain;
        if (cnt++ < 12) {
            const char* dn = diffuse ? (diffuse->Name() ? diffuse->Name() : "?") : "<null>";
            fprintf(stderr,
                "[RB3_DRAWRECT_DBG] rect=(%.1f,%.1f,%.1f,%.1f) mod=(%.3f,%.3f,%.3f,%.3f) "
                "matCol=(%.3f,%.3f,%.3f,%.3f) diffuse='%s' colorMod=%d rtActive=%d fmt=%d hasDepth=%d\n",
                rect.x, rect.y, rect.w, rect.h,
                ub.mod[0], ub.mod[1], ub.mod[2], ub.mod[3],
                matCol.red, matCol.green, matCol.blue, matCol.alpha,
                dn, colorMod, rtPass ? 1 : 0, (int)fmt, hasDepth ? 1 : 0);
        }
    }
}
