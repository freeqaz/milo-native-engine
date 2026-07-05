// RB3 additive-halo-only highway gem-bloom pass — extracted VERBATIM from
// BandRnd:: methods in platform/Rnd_Wgpu_RB3.cpp (W1.4.S1). RB3-only TU
// (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3). Pure MOVE: the capture-and-replay
// gem bloom below is copied token-for-token (it captures the LIVE
// mSceneBindGroup HANDLE per draw; a dynamicOffsetCount mismatch on replay
// silently discards the command buffer). See RB3HaloPass.h / W1.4 PLAN.
//
// Moved defs: HighwayBloomEnabled, IsHaloSourceMat, EnsureHaloTarget,
// EnsureHaloBlitPipeline, CompositeHaloBloom (all BandRnd:: members already
// declared in Rnd_Wgpu_RB3.h). The kRB3HaloBlitShaderSource WGSL include moves
// with them.

#include "platform/Rnd_Wgpu_RB3.h"

#include "rndobj/Mat.h"
#include "rndobj/Tex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ===========================================================================
// P1 additive-halo-only highway gem bloom (DEFAULT-ON; opt out RB3_HIGHWAY_BLOOM_OFF).
// DESIGN B: capture-and-replay, NOT the rejected redirect (Design A). The live
// highway pass (game.cam) is NEVER touched — DrawMesh only CAPTURES a per-draw
// replay record for each halo-source mesh (the live pose-baked scene bind group
// handle + mat/obj/bone bind groups + vbuf/ibuf). At EndFrame, CompositeHaloBloom
// replays those draws into a transparent buffer, blooms it, and ADDITIVE-blits
// ONLY the blurred halo onto mFrameView. The base track is unaffected, so
// RB3_HIGHWAY_BLOOM_BLEND=0 is visually identical to OFF (negative control).
//
// When RB3_HIGHWAY_BLOOM_OFF=1, HighwayBloomEnabled() returns false and every site
// (the DrawMesh capture, the EndFrame composite) is a no-op — byte-identical to
// the pre-bloom path. The halo is confined to the emissive gem cores + now-bar
// (IsHaloSourceMat): the full-quad track surface and HUD meter-glass are excluded
// so the dark track + HUD are never washed.
// ===========================================================================
bool BandRnd::HighwayBloomEnabled() {
    // DEFAULT-ON (gems/now-bar additive bloom — retail-accurate, confined to the
    // emissive gem cores + strike line, never the track surface or HUD). Opt out
    // via RB3_HIGHWAY_BLOOM_OFF=1 (mirrors RB3_TRACK_LIGHT_OFF / RB3_VENUE_LIGHT_OFF).
    static int s = -1;
    if (s < 0) { const char* e = getenv("RB3_HIGHWAY_BLOOM_OFF"); s = (e && e[0] && e[0] != '0') ? 0 : 1; }
    return s != 0;
}

// Halo-source classifier. The halo must be CONFINED to the small, bright emissive
// gem cores (prism_mat, emisMap=prism_gem_emissive, mult 1.0). Exclusions keep it
// from washing the scene:
//   - surface.mat (the highway watermark) is also emissive but is a FULL QUAD;
//     blooming a full quad washes the whole track + lifts the black point. Exclude
//     it by name — it's the one full-plane emissive on the highway.
//   - gem_smasher_glow.mat (the held-fret / now-bar strike glow): originally a halo
//     source, but it is a now-bar-sized plate textured with a SOFT radial glow, not
//     a small gem core. Blooming it (then compounded by the wave-2 ×2 now-bar boost)
//     produced a GIANT WHITE SPHERE hovering above the now-bar that occluded gems in
//     bloom-heavy venues (wave-2 regression, repro: 20th Century Boy ~49.5s). The
//     glow already draws additively at authored intensity in the main pass — the
//     bloom only over-amplified it into the sphere. Exclude it so the held-fret glow
//     stays its small per-slot colour without the halo blowout. (Opt back in via
//     RB3_SMASHER_HALO=1 for A/B; default-off.)
//   - The additive-blend test (kBlendAdd/kBlendSrcAlphaAdd) was dropped: its only
//     unique catches were the HUD overdrive/streak meter-glass lenses, which bloom
//     into the HUD. The now-bar is already selected by its emissive map, so the
//     blend test added only spill.
// Safe only under the game.cam guard at the call site (other cams never reach this).
bool BandRnd::IsHaloSourceMat(RndMat* mat) {
    if (!mat) return false;
    if ((RndTex*)mat->mEmissiveMap == nullptr || mat->mEmissiveMultiplier <= 0.0f) return false;
    const char* mn = mat->Name();
    if (mn && std::strstr(mn, "surface")) return false;   // full-quad track plane — would wash
    if (mn && std::strstr(mn, "gem_smasher_glow")) {       // now-bar plate — blooms to a white sphere
        static int sSmasherHalo = -1;
        if (sSmasherHalo < 0) {
            const char* e = getenv("RB3_SMASHER_HALO");
            sSmasherHalo = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (!sSmasherHalo) return false;
    }
    return true;
}

// (Re)create the halo replay target at w x h. Same format/usage as
// EnsureIntermediate (mTargetFmt, RenderAttachment | TextureBinding) so the
// replay can render into it and CompositeHaloBloom can sample it. Sized to the
// window (matches mDepthView), NOT mIntermediate*. Early-out if unchanged.
void BandRnd::EnsureHaloTarget(int w, int h) {
    if (!mGpuReady || w <= 0 || h <= 0) return;
    if (mHaloTex && mHaloView && mHaloWidth == w && mHaloHeight == h) return;
    wgpu::TextureDescriptor td{};
    td.label = "RB3HaloBloomTarget";
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = mTargetFmt;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    wgpu::Texture t = mGpu.Device().CreateTexture(&td);
    if (!t) return;
    mHaloTex = t;
    mHaloView = t.CreateView();
    mHaloWidth = w;
    mHaloHeight = h;
}

// A minimal WGSL module: vs_fullscreen (fullscreen triangle, no vbuf) + fs_blit.
// OUTER-HALO-ONLY composite: srcTex@0 is the BLOOMED halo (blurred, thresholded);
// rawTex@2 is the pre-blur source footprint (mHaloView). fs_blit emits
//   max(bloom - raw, 0) * blend
// so inside the gem body (raw bright) the contribution cancels to ~0 and the gem
// keeps its own saturated base-pass color, while the spread glow around it
// (raw==0, bloom>0) survives and is ADDITIVELY laid over the untouched framebuffer.
// Builds ONLY the ADDITIVE pipeline (color & alpha One/One) — the premultiplied-OVER
// pipeline of the rejected Design A is dropped.
static const char* kRB3HaloBlitShaderSource =
#include "gfx/Shaders/rb3_halo_blit.wgsl.inc"
;

void BandRnd::EnsureHaloBlitPipeline() {
    if (mHaloBlitReady) return;
    auto& dev = mGpu.Device();

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kRB3HaloBlitShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    mHaloBlitShader = dev.CreateShaderModule(&smDesc);

    // group 0: srcTex@0 (bloomed halo, Float 2D), sampler@1 (Filtering),
    // rawTex@2 (pre-blur source footprint, Float 2D), blendUB@3 (uniform).
    wgpu::BindGroupLayoutEntry entries[4] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[3].binding = 3;
    entries[3].visibility = wgpu::ShaderStage::Fragment;
    entries[3].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[3].buffer.minBindingSize = 16;  // BlendUB: blend + 3 pad floats

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 4;
    bglDesc.entries = entries;
    mHaloBlitBGL = dev.CreateBindGroupLayout(&bglDesc);

    // Blend uniform buffer (16B; blend float + 3 pad). Updated per composite via
    // Queue.WriteBuffer so RB3_HIGHWAY_BLOOM_BLEND stays live-tunable.
    wgpu::BufferDescriptor blendBufDesc{};
    blendBufDesc.size = 16;
    blendBufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mHaloBlendBuf = dev.CreateBuffer(&blendBufDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &mHaloBlitBGL;
    mHaloBlitPL = dev.CreatePipelineLayout(&plDesc);

    // ONLY the additive pipeline (color One/One, alpha One/One).
    wgpu::BlendState bs{};
    bs.color.srcFactor = wgpu::BlendFactor::One;
    bs.color.dstFactor = wgpu::BlendFactor::One;
    bs.color.operation = wgpu::BlendOperation::Add;
    bs.alpha.srcFactor = wgpu::BlendFactor::One;
    bs.alpha.dstFactor = wgpu::BlendFactor::One;
    bs.alpha.operation = wgpu::BlendOperation::Add;

    wgpu::ColorTargetState ct{};
    ct.format = mTargetFmt;
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = mHaloBlitShader;
    frag.entryPoint = "fs_blit";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = mHaloBlitPL;
    pd.vertex.module = mHaloBlitShader;
    pd.vertex.entryPoint = "vs_fullscreen";
    pd.fragment = &frag;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.depthStencil = nullptr;
    pd.multisample.count = 1;
    mHaloAddPipeline = mGpu.Device().CreateRenderPipeline(&pd);

    mHaloBlitReady = true;
}

// EndFrame composite (Design B): replay the captured halo-source draws into the
// transparent halo buffer, bloom it, and ADDITIVE-blit ONLY the blurred halo
// onto mFrameView (LoadOp::Load — base track preserved). Same encoder, after
// mPass.End() and before mEncoder.Finish(). Clears mHaloDraws (keeps capacity).
void BandRnd::CompositeHaloBloom() {
    if (!mGpuReady || mHaloDraws.empty() || !mFrameView) { mHaloDraws.clear(); return; }

    // Read tunables up front so the neg-control (BLEND=0) can short-circuit to a
    // TRUE no-op. The engine's BloomPass does NOT scale its threshold/blur output
    // by the Run `intensity` arg (only the upsample mip-merge uses a SEPARATE
    // weight), so OutputView() is non-zero even at intensity=0 — i.e. passing
    // blend=0 to Run() would still add the base-mip halo. To honor the contract
    // "RB3_HIGHWAY_BLOOM_BLEND=0 is visually identical to OFF", skip the replay +
    // bloom + composite entirely when blend<=0 (touches NOTHING on mFrameView).
    float thresh = 0.55f;
    float blend = 0.7f;
    {
        const char* t = getenv("RB3_HIGHWAY_BLOOM_THRESH");
        if (t && t[0]) thresh = (float)atof(t);
        const char* b = getenv("RB3_HIGHWAY_BLOOM_BLEND");
        if (b && b[0]) blend = (float)atof(b);
    }
    if (blend <= 0.0f) {
        if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
            fprintf(stderr, "[RB3_HALOBLOOM] f%d blend=0 -> no-op (identical to OFF)\n",
                    mFrameCount);
        mHaloDraws.clear();
        return;
    }

    // Match mDepthView's live size: the replay pass below pairs mHaloView with
    // mDepthView, so the halo target must be the same size as the depth buffer
    // (WindowWidth/Height can be stale after a web canvas resize — see BeginFrame).
    int W = mDepthWidth, H = mDepthHeight;
    if (W <= 0 || H <= 0) { W = mGpu.WindowWidth(); H = mGpu.WindowHeight(); }
    EnsureHaloTarget(W, H);
    EnsureHaloBlitPipeline();
    if (!mHaloView || !mHaloAddPipeline || !mHaloBlitBGL || !mDepthView) {
        mHaloDraws.clear();
        return;
    }

    // (b) Replay pass: clear the halo buffer TRANSPARENT (only halo-source pixels
    //     become non-zero) and clear depth (the captured draws were authored with
    //     depth on). Replay each captured draw verbatim against its pose-baked
    //     scene bind group — NO dynamic offset (the bind group pins mSceneOffset
    //     directly, so dynamicOffsetCount must be 0 to match the live draw).
    {
        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mHaloView;
        colorAtt.loadOp = wgpu::LoadOp::Clear;
        colorAtt.storeOp = wgpu::StoreOp::Store;
        colorAtt.clearValue = {0, 0, 0, 0};

        wgpu::RenderPassDepthStencilAttachment depthAtt{};
        depthAtt.view = mDepthView;
        depthAtt.depthLoadOp = wgpu::LoadOp::Clear; depthAtt.depthStoreOp = wgpu::StoreOp::Store;
        depthAtt.depthClearValue = 1.0f;
        depthAtt.stencilLoadOp = wgpu::LoadOp::Clear; depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
        depthAtt.stencilClearValue = 0;

        wgpu::RenderPassDescriptor rp{};
        rp.label = "BandHaloReplay";
        rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
        rp.depthStencilAttachment = &depthAtt;

        wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
        for (const HaloDraw& d : mHaloDraws) {
            pass.SetPipeline(d.pipe);
            pass.SetBindGroup(0, d.scene, 0, nullptr);
            pass.SetBindGroup(1, d.mat, 0, nullptr);
            pass.SetBindGroup(2, d.obj, 0, nullptr);
            pass.SetBindGroup(3, d.bone, 0, nullptr);
            pass.SetVertexBuffer(0, d.vbuf, 0, WGPU_WHOLE_SIZE);
            pass.SetIndexBuffer(d.ibuf, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
            pass.DrawIndexed(d.indexCount, 1, 0, 0, 0);
        }
        pass.End();
    }

    // (c) Bloom the halo buffer (its own BloomPass instance / mip chain).
    wgpu::TextureView haloView;
    {
        Hmx::Color tint(1.f, 1.f, 1.f, 1.f);
        mHaloBloom.Run(mEncoder, mHaloView, mHaloWidth, mHaloHeight,
                       blend, thresh, tint, mGpu);
        if (mHaloBloom.HasOutput()) haloView = mHaloBloom.OutputView();
    }

    // (d) OUTER-HALO-ONLY additive composite: one fullscreen pass on mFrameView
    //     (LoadOp::Load → keep the base frame), no depth. The blit shader emits
    //     max(bloom - raw, 0) * blend, sampling BOTH the bloomed halo (haloView)
    //     and the un-blurred source footprint (mHaloView) — so the gem BODY
    //     cancels to ~0 (keeps its saturated base-pass color) and only the OUTER
    //     glow is added. With blend==0 we already returned above (no-op == OFF).
    {
        // blend → blendUB@3 (live-tunable via RB3_HIGHWAY_BLOOM_BLEND).
        float blendUB[4] = { blend, 0.0f, 0.0f, 0.0f };
        mGpu.Queue().WriteBuffer(mHaloBlendBuf, 0, blendUB, sizeof(blendUB));

        wgpu::RenderPassColorAttachment colorAtt{};
        colorAtt.view = mFrameView;
        colorAtt.loadOp = wgpu::LoadOp::Load;
        colorAtt.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor rp{};
        rp.label = "BandHaloBloomComposite";
        rp.colorAttachmentCount = 1; rp.colorAttachments = &colorAtt;
        rp.depthStencilAttachment = nullptr;

        wgpu::RenderPassEncoder pass = mEncoder.BeginRenderPass(&rp);
        if (haloView) {
            wgpu::BindGroupEntry bge[4] = {};
            bge[0].binding = 0; bge[0].textureView = haloView;     // bloomed halo
            bge[1].binding = 1; bge[1].sampler = mSampler;
            bge[2].binding = 2; bge[2].textureView = mHaloView;    // raw source footprint
            bge[3].binding = 3; bge[3].buffer = mHaloBlendBuf; bge[3].size = 16;
            wgpu::BindGroupDescriptor bgd{};
            bgd.layout = mHaloBlitBGL; bgd.entryCount = 4; bgd.entries = bge;
            wgpu::BindGroup bg = mGpu.Device().CreateBindGroup(&bgd);
            pass.SetPipeline(mHaloAddPipeline);
            pass.SetBindGroup(0, bg, 0, nullptr);
            pass.Draw(3);
        }
        pass.End();
    }

    if (getenv("RB3_RENDER_DBG") || getenv("RB3_TIER2_DBG"))
        fprintf(stderr, "[RB3_HALOBLOOM] f%d %dx%d draws=%zu thresh=%.2f blend=%.2f halo=%d\n",
                mFrameCount, mHaloWidth, mHaloHeight, mHaloDraws.size(), thresh, blend,
                haloView != nullptr);

    // (e) Done — keep capacity for next frame.
    mHaloDraws.clear();
}
