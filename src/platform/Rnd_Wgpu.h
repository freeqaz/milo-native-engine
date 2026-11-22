// DC3 Native Port — WebGPU Renderer Header
// WgpuRnd (extends the shape's renderer base: NgRnd on DC3-shaped rndobj) and,
// where the shape has the NG shader layer, WgpuShaderMgr (extends RndShaderMgr).

#pragma once

#include "gfx/FrameCost.h"
#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "gfx/ShadowPass.h"
#include "gfx/PostProcPass.h"
#include "gfx/DisplayRamp.h"
#include "gfx/DrawRect2D.h"
#include "gfx/Canvas2D.h"
#include "gfx/PointTestPass.h"
#include "gfx/PointTestStandIns.h"
#include "gfx/SpotBeamPass.h"
#include "gfx/ContactShadowPass.h"
#include "gfx/TexBlendPass.h"
#include "gfx/UniformStructs.h"
#include "gfx/UniformRingBuffer.h"
#include "gfx/BindGroupCache.h"
#include "platform/rndshape/RndShape.h"
#include "platform/MeshPipelineKey.h"
#include "platform/PointTestHook.h"
#include "platform/SpotBeamHook.h"
#include "platform/ContactShadowHook.h"
#include "platform/TexBlendHook.h"
#ifdef MILO_RNDOBJ_SHAPE_HAS_NGRND
#include "rndobj/ShaderMgr.h"
#endif

#include "gfx/VideoEncoder.h"

#include <string>
#include <vector>
#include <webgpu/webgpu_cpp.h>

// UniformRingBuffer moved to gfx/UniformRingBuffer.h (W1.5, shared gfx-core TU).

// ============================================================================
// WgpuShaderMgr — captures SetVConstant/SetPConstant into staging area
// ============================================================================

#ifdef MILO_RNDOBJ_SHAPE_HAS_NGRND
class WgpuShaderMgr : public RndShaderMgr {
public:
    WgpuShaderMgr() {}
    virtual ~WgpuShaderMgr() {}

    void Init() override;
    void Terminate() override;

    // For Tier 1, most constants are captured but not used —
    // scene/material/object uniforms are written directly from engine state
    void SetVConstant(VShaderConstant, const Hmx::Matrix4&) override {}
    void SetVConstant4x3(VShaderConstant, const Hmx::Matrix4&) override {}
    void SetVConstant(VShaderConstant, RndTex*) override {}
    // Vector4 vertex constants are kept: the DC3 shape's retail light model
    // reads the box map NgEnviron::UpdateApproxLighting uploads (c80..c85)
    // at draw time, the register the image's standard.vs reads
    // (rndshape::FillMeshApproxLighting, RndShape_DC3.h). Storing them
    // changes no render on its own.
    void SetVConstant(VShaderConstant reg, const Vector4& v) override {
        if ((unsigned)reg < kNumVConstants) mVConstants[reg] = v;
    }
    const Vector4& VConstant(int reg) const { return mVConstants[reg]; }
    static constexpr unsigned kNumVConstants = 256;
    void SetVConstant(VShaderConstant, const float*, unsigned int) override {}
    void SetVConstant(VShaderConstant, int) override {}
    void SetVConstant(VShaderConstant, bool) override {}
    void SetPConstant(PShaderConstant, const Hmx::Matrix4&) override {}
    void SetPConstant(PShaderConstant, RndCubeTex*) override {}
    // Vector4 pixel constants are kept too, for the same reason: the DC3
    // shape's self-shadow reads c107/c108 as NgRnd::SetShadowMap uploads them
    // (rndshape::CapturedPConstant). Storing them changes no render on its own.
    void SetPConstant(PShaderConstant reg, const Vector4& v) override {
        if ((unsigned)reg < kNumPConstants) mPConstants[reg] = v;
    }
    const Vector4& PConstant(int reg) const { return mPConstants[reg]; }
    static constexpr unsigned kNumPConstants = 256;
    void SetPConstant(PShaderConstant, RndTex*) override {}
    void SetPConstant(PShaderConstant, int) override {}
    void SetPConstant(PShaderConstant, bool) override {}
    void SetPConstant4x3(PShaderConstant, const Hmx::Matrix4&) override {}

protected:
    RndShaderProgram* NewShaderProgram() override { return nullptr; }

private:
    Vector4 mVConstants[kNumVConstants] = {};
    Vector4 mPConstants[kNumPConstants] = {};
};
#endif // MILO_RNDOBJ_SHAPE_HAS_NGRND

// ============================================================================
// WgpuRnd — WebGPU renderer extending WgpuRndBase (see platform/rndshape/)
// ============================================================================

class WgpuRnd : public WgpuRndBase {
public:
    WgpuRnd() {}
    virtual ~WgpuRnd() {}

    void Init() override;
    // Deferred GPU resource setup — called after mGpu.IsReady() on web,
    // or inline from Init() on native.  Sets up pipelines, ring buffers,
    // depth texture, default textures, shadow/post-proc passes.
    // Idempotent — safe to call multiple times (no-op after first).
    void InitGpuResources();
    bool GpuResourcesReady() const { return mGpuResourcesReady; }
    void Terminate() override;
    void Clear(unsigned int, const Hmx::Color&) override;
    void BeginDrawing() override;
    void EndDrawing() override;
    void MakeDrawTarget() override;
    void SetViewport(const Viewport& v) override;

    // Screen-space 2D drawing (NgRnd override)
    void DrawRect(const Hmx::Rect&, RndMat*, ShaderType, const Hmx::Color&,
                  const Hmx::Color*, const Hmx::Color*) override;
    // Base Rnd override: retail's kDrawRectShader colour model on Xbox shapes
    // (DrawRectMaterialColor), then the screen-space path above.
    void DrawRect(const Hmx::Rect& r, const Hmx::Color& c, RndMat* m,
                  const Hmx::Color* tr, const Hmx::Color* bl) override;

    // On web, returns an owned offscreen texture; on desktop, the swapchain surface.
    // All rendering (post-proc output, overlay passes) targets this view.
    // Avoids LoadOp::Load on web swapchain surfaces (unreliable content preservation).
    wgpu::TextureView& FrameTarget() {
#ifdef __EMSCRIPTEN__
        return mFrameResolvedView;
#else
        return mFrameView;
#endif
    }

    // The texture behind FrameTarget().
    wgpu::Texture FrameTargetTexture() {
#ifdef __EMSCRIPTEN__
        return mFrameResolvedTex;
#else
        return mGpu.IsHeadless() ? mGpu.HeadlessTex() : mGpu.SurfaceTexture();
#endif
    }

    // Accessors for Mesh_Wgpu.cpp / Tex_Wgpu.cpp
    GpuDevice& Gpu() { return mGpu; }
    // Called once per drawn frame after its command buffer is submitted and
    // before it is presented: the one point where a windowed frame can be read
    // back (GpuDevice::ReadbackFrame). A consumer's frame grab (dc3's
    // /api/screenshot) registers here. nullptr clears it.
    using FrameReadyCallback = void (*)();
    void SetFrameReadyCallback(FrameReadyCallback cb) { mFrameReadyCallback = cb; }
    PipelineManager& Pipelines() { return mPipelines; }
    // The open pass, for a draw about to be recorded into it. Pending 2D
    // canvas quads (gfx/Canvas2D.h) are recorded first, so the canvas and
    // every other draw land in the pass in call order.
    wgpu::RenderPassEncoder& CurrentPass() {
        if (mCanvas2D.HasPending()) mCanvas2D.Flush();
        return mPass;
    }
    // The 2D canvas (gfx/Canvas2D.h; TheCanvas2D() returns this one).
    Canvas2D& Canvas() { return mCanvas2D; }
    // For Canvas2D's own draws: the pass without the flush above.
    wgpu::RenderPassEncoder& PassForCanvas2D() { return mPass; }
    // The open pass's viewport as ApplyViewport set it, in pixels of the
    // pass's target: x, y, w, h.
    void PassViewportPixels(float out[4]) const;
    // The scissor the renderer keeps on the open pass, in pixels of its
    // target (x, y, w, h): the clip-plane rectangle, or the whole target.
    void PassScissor(int out[4]) const;
    bool IsInPass() const { return mInPass; }
    wgpu::TextureFormat CurrentTargetFormat() const { return mCurrentTargetFormat; }
    uint32_t CurrentSampleCount() const { return mCurrentSampleCount; }
    // np-look47: the sample count a texture pass draws with when the look's
    // texPassMsaa reaches its target -- the frame's (kMSAASamples).
    static constexpr uint32_t kTexPassMsaaSamples = 4;
    bool CurrentPassHasDepth() const { return mCurrentPassHasDepth; }
    // The pass states a mesh can be drawn in, for a warm that runs outside any
    // pass (MeshPipelineKey.h): the world (BeginFramePass), the HUD overlay
    // (BeginOverlayPass), and a render target (the TexturePass) with or
    // without depth.
    MeshPassContext WorldPassContext(bool reflection) const;
    MeshPassContext OverlayPassContext() const;
    // likeFrame: a target the look draws like the frame (np-look47,
    // LookSettings::texPassMsaa; RndTex type kRendered), whose pass then has the
    // frame's sample count. The self-shadow caster pass never is.
    MeshPassContext TexturePassContext(bool depth, bool likeFrame = false) const;
    // Compile now the pipeline DrawRect asks for with `mat` in `ctx` (a
    // load-time warm, DrawRect2D::Warm). Returns the pipelines created.
    int WarmRectPipelines(RndMat* mat, const MeshPassContext& ctx) {
        return mDrawRect2D.Warm(mGpu, mPipelines, mat, ctx.targetFormat, ctx.hasDepth,
                                ctx.toTexture, ctx.sampleCount);
    }
    // ... for a rect drawn with scratch material state (`blend`, textured or
    // not) into a target with no depth (DrawRect2D::Warm's second form).
    int WarmRectPipelines(WgpuBlend blend, bool hasTex, const MeshPassContext& ctx) {
        return mDrawRect2D.Warm(mGpu, mPipelines, blend, hasTex, ctx.targetFormat, ctx.toTexture,
                                ctx.sampleCount);
    }
    // GetViewport() in the current frame target's pixels (render scale).
    Viewport FrameViewport(const Viewport& v) const;
    // The frame has left its world (FlushPostProcessingForOverlay ran): what
    // draws now is UI/HUD, or a render target that UI draws. Reset by the
    // frame's clearing BeginFramePass.
    bool FrameInOverlay() const { return mFrameInOverlay; }
    uint32_t CurrentTargetWidth() const { return mCurrentTargetWidth; }
    uint32_t CurrentTargetHeight() const { return mCurrentTargetHeight; }
    int FrameID() const { return mFrameID; }

    // Scene bind group (group 0) — updated when camera changes
    wgpu::BindGroup& SceneBindGroup() { return mSceneBindGroup; }
    wgpu::Buffer& SceneBuffer() { return mSceneRing.Buffer(); }
    uint32_t SceneOffset() const { return mLastSceneOffset; }
    // SceneUniforms.viewProj as last written (column-major), for the draw log.
    const float* LastSceneViewProj() const { return mLastSceneViewProj; }
    // SceneUniforms.specularAA as last written (0 in the HUD overlay phase).
    float LastSceneSpecularAA() const { return mLastSceneSpecularAA; }
    // The materialLodBias the last WriteSceneUniforms wrote (tests).
    float LastSceneMaterialLodBias() const { return mLastSceneMaterialLodBias; }
    // The text / other-HUD-art biases the last WriteSceneUniforms wrote (np-look34, tests).
    float LastSceneUiLodBias() const { return mLastSceneUiLodBias; }
    float LastSceneUiArtLodBias() const { return mLastSceneUiArtLodBias; }
    // The cutoutSampleTest the last WriteSceneUniforms wrote (tests; np-look26).
    float LastSceneCutoutSampleTest() const { return mLastSceneCutoutSampleTest; }
    // The cutout alpha test per sample for the pass now open, 1 or 0
    // (LookSettings::cutoutSampleTest): 0 in the HUD overlay phase.
    float PassCutoutSampleTest() const;
    // The world material LOD bias for the pass now open: the look's rule
    // (milo::MaterialLodBiasAt) at the frame's render scale, or at 1 for a
    // texture target.
    float PassMaterialLodBias() const;
    void EnsureSceneUniformsCurrent();  // call before drawing — re-uploads if camera changed

    // Default textures
    wgpu::TextureView& WhiteTexView() { return mWhiteTexView; }
    wgpu::TextureView& FlatNormalTexView() { return mFlatNormalTexView; }
    wgpu::TextureView& BlackTexView() { return mBlackTexView; }
    wgpu::TextureView& BlackCubeTexView() { return mBlackCubeTexView; }
    wgpu::Sampler& DefaultSampler() { return mDefaultSampler; }

    // Material texture views for bind group creation
    struct MaterialTexViews {
        wgpu::TextureView diffuse;
        wgpu::TextureView normal;
        wgpu::TextureView specular;
        wgpu::TextureView emissive;
        wgpu::TextureView rim;
        wgpu::TextureView environCube;
        wgpu::TextureView normDetail;
        wgpu::TextureView refractNormal;   // binding 11 (rndshape::MatRefract)
    };

    // Bind groups for the mesh path (PLAN H-30). The uniform binding of each
    // takes a dynamic offset (PipelineManager::SetDynamicUniformOffsets): pass
    // the ring offset Write() returned to SetBindGroup. The groups come from
    // mBindGroupCache, keyed by every resource they bind, so a draw that binds
    // what an earlier draw bound reuses its group instead of creating one.
    // Material (group 1): the material ring + the material's views/samplers.
    wgpu::BindGroup MaterialBindGroup(const MaterialTexViews& texViews,
                                      wgpu::Sampler& diffuseSampler, wgpu::Sampler& mapSampler);
    // Object (group 2): the object ring.
    wgpu::BindGroup ObjectBindGroup();
    // Bone (group 3), skinned meshes: the bone ring.
    wgpu::BindGroup BoneBindGroup();

    BindGroupCache& BindGroups() { return mBindGroupCache; }
    BindGroupCache& PostBindGroups() { return mPostBindGroupCache; }
    // A texture view is being released (RndTex destroyed, render target
    // recreated): drop the cached bind groups that bind it.
    void ForgetTextureView(const wgpu::TextureView& view) {
        if (view) {
            mBindGroupCache.Forget(view.Get());
            mPostBindGroupCache.Forget(view.Get());  // e.g. a post-process noise map
        }
    }

    // Ring buffers for per-draw uniforms
    UniformRingBuffer& MaterialRing() { return mMaterialRing; }
    UniformRingBuffer& ObjectRing() { return mObjectRing; }
    UniformRingBuffer& BoneRing() { return mBoneRing; }

    // Shadow mapping — accessed by Mesh_Wgpu.cpp (delegates to ShadowPass)
    bool InShadowPass() const { return mShadowPass.InShadowPass(); }
    wgpu::RenderPassEncoder& ShadowRenderPass() { return mShadowPass.Pass(); }
    wgpu::RenderPipeline& ShadowStaticPipeline() { return mShadowPass.StaticPipeline(); }
    wgpu::RenderPipeline& ShadowSkinnedPipeline() { return mShadowPass.SkinnedPipeline(); }
    wgpu::BindGroupLayout& ShadowObjectBGL() { return mShadowPass.ObjectBGL(); }
    wgpu::BindGroupLayout& ShadowBoneBGL() { return mShadowPass.BoneBGL(); }
    wgpu::TextureView& ShadowDepthView() { return mShadowPass.DepthView(); }
    wgpu::Sampler& ShadowSampler() { return mShadowPass.Sampler(); }
    bool ShadowAvailable() const { return mShadowPass.Available(); }

    void SelectRenderTarget(RndTex* tex);
    void FinishRenderTarget(RndTex* tex);
    RndTex* ActiveTargetTex() const { return mActiveTargetTex; }

    // Rnd::EndWorld's world-end step, on every shape: retail DxRnd::DoWorldEnd
    // runs Rnd::DoWorldEnd, then DoPointTests, then SavePreBuffer. NgRnd has no
    // DoWorldEnd of its own; retail's lives in the platform renderer, which
    // WgpuRnd stands in for.
    void DoWorldEnd() override;

    // Retail DxRnd::SavePreBuffer's colour half: copy the world, as it stands
    // at world end, into the pre-process buffer (mPreTex). Retail resolves the
    // world's colour into mPreProcessBuffer (and its depth into
    // mFrontBufferDepth, which nothing here reads). See
    // dc3-backend-for-rb3-wii.md section 22.
    void SavePreBuffer();
    // The copy itself, between passes (no pass may be open).
    void CopyWorldToPreBuffer();
    // The frame a refracting material samples (binding 12): retail
    // DxRnd::GetCurrentFrameTex, i.e. the pre-process buffer until this
    // frame's world post-processing has run, then its output; black before
    // the first SavePreBuffer.
    wgpu::TextureView& RefractFrameView();

    // Flare point tests (platform/PointTestHook.h): retail DxRnd::DoPointTests
    // on occlusion queries. QueuePointTest holds a test for this frame's world
    // end; RunPointTests, at world end (DoWorldEnd; or, for a frame that never
    // ends its world, the first thing EndDrawing does, as retail's
    // Rnd::EndDrawing ends the world), first delivers the answers to the tests
    // issued the frame before if their readback has finished, then draws this
    // frame's against the world's depth. FinishPointTestReadback, at the end of
    // EndDrawing (after the submit), waits for any of the earlier frames'
    // answers that were not ready, so every answer still arrives one frame after
    // its test, as retail's do, without the CPU waiting on an idle GPU.
    bool QueuePointTest(const NativePointTest& test);
    void CancelPointTests(const void* key);
    void RunPointTests();
    void FinishPointTestReadback();
    int PendingPointTests() const { return (int)mPointTestQueue.size(); }
    int PointTestsInFlight() const { return mPointTestPass.InFlight(); }
    // FinishPointTestReadback calls that blocked on a readback (never, under a
    // shape whose rndshape::kPointTestWaitAfterSubmit is false), and the
    // stand-in answers delivered for late ones (PointTestStandIns).
    uint64_t PointTestEndWaits() const { return mPointTestEndWaits; }
    uint64_t PointTestStandInsDelivered() const { return mPointTestStandInsDelivered; }
    // Counts an answer for the frame-times log (MILO_FRAME_TIMES).
    void NotePointTestAnswer(const PointTestPass::Answer& answer);

    // RndTexBlender composition (platform/TexBlendHook.h): retail
    // RndTexBlender::DrawShowing's base rect and unwrapped controller meshes,
    // recorded into the frame's encoder as one pass over the output texture.
    // A frame pass open on entry is resumed (loaded) afterwards.
    bool ComposeTexBlend(RndTex* output, RndTex* base, const NativeTexBlendLayer* layers,
                         int count);
    int TexBlendsComposed() const { return mTexBlendsComposed; }
    // Creates the pipelines ComposeTexBlend draws `output` with (a load-time
    // warm, PLAN H-104); returns how many were created.
    int WarmTexBlendPipelines(RndTex* output);

    // Volumetric spotlight beams (platform/SpotBeamHook.h, gfx/SpotBeamPass.h):
    // retail NgSpotlightDrawer::DoPost. SubmitSpotBeams keeps this frame's
    // beams, with the camera current at submit; FlushWorldPost draws them
    // against the world's depth and hands the blurred result to the RB3
    // composite. Beams not drawn by the frame's end are dropped.
    bool SubmitSpotBeams(const NativeSpotBeamFrame& frame, const NativeSpotBeam* beams, int count);
    int SpotBeamsDrawn() const { return mSpotBeamsDrawn; }

    // Crowd contact shadows (platform/ContactShadowHook.h, gfx/ContactShadowPass.h;
    // DC3 enhanced look, LookSettings::crowdContactShadow). A shadow is kept
    // only while the main frame pass is open (no render target, shadow map or
    // HUD overlay), transformed with the camera current at queue time, and
    // drawn at the next camera change (before the deferred transparent draws)
    // or when the pass ends: after the opaque world at that camera.
    bool WantsContactShadows() const;
    bool QueueContactShadow(const NativeContactShadow& s);
    void FlushContactShadows();
    // Shadows drawn so far this frame / in the last finished frame.
    int ContactShadowsDrawnThisFrame() const { return mContactShadowsDrawn; }
    int ContactShadowsDrawnLastFrame() const { return mContactShadowsDrawnLast; }
    // True when the main frame pass's contact shadow pipeline exists already
    // (PrewarmFixedKeyPipelines under a look that draws them).
    bool ContactShadowPipelineReady() const {
        return mContactShadowPass.Ready(mGpu.SurfaceFormat(), kMSAASamples);
    }

    // Compiles the pipelines whose keys are fixed once the device is up (depth
    // of field, spot beams, point tests, shadow casters); DC3 calls it at init.
    // Returns how many it created: 0 when they all exist with the keys a frame
    // will ask for.
    uint64_t PrewarmFixedKeyPipelines();
    const PostProcPass& PostProc() const { return mPostProcPass; }

    // RndDrawable::mClipPlanes (Rnd::PushClipPlanes/PopClipPlanes around a
    // drawable's DrawShowing). Retail DxRnd::PushClipPlanesInternal turns each
    // plane transform into a D3D user clip plane: the plane through the
    // transform's origin with normal -z, keeping the side that normal points
    // at, in the current camera's clip space. WebGPU has no user clip planes
    // in core, so each plane becomes one edge of a scissor rectangle instead:
    // the plane's line on screen at the depth of its origin. That is exact for
    // a plane the camera sees edge-on (its screen line is vertical or
    // horizontal), which is what a UI box's clip planes are (the main menu's
    // motd.lbl marquee: motd_clip_left/right.trans). A plane whose screen line
    // is oblique cannot be a scissor edge and does not clip (one log line).
    // Only shapes whose Rnd has the hooks (MILO_RNDOBJ_SHAPE_HAS_CLIP_PLANES);
    // elsewhere the clip stack below stays empty and every scissor is full.
#ifdef MILO_RNDOBJ_SHAPE_HAS_CLIP_PLANES
    void PushClipPlanesInternal(ObjPtrVec<RndTransformable>& planes) override;
    void PopClipPlanesInternal(ObjPtrVec<RndTransformable>& planes) override;
#endif
    // The scissor the innermost pushed planes give, in pixels of the target
    // current when they were pushed (x, y, w, h; w or h may be 0). False when
    // no clip plane is in effect.
    bool ClipScissor(int rect[4]) const;
    int ClipPlaneDepth() const { return (int)mClipStack.size(); }

private:
    struct ClipRect { float x0, y0, x1, y1; };
    std::vector<ClipRect> mClipStack;
    bool mClipScissorSet = false; // the open pass carries a non-full scissor
    void ApplyClipScissor();
    void ApplyViewport();
    // The viewport a camera select sets on DC3 (RndCam::Select) and on the Wii
    // (WiiCam::Select): mScreenRect over the current target, depth mZRange.
    // Called by WgpuRnd only for shapes whose RndCam::Select does not do it
    // (rndshape::kCamSelectSetsViewport).
    void ApplyCameraViewport(RndCam* cam);
    void BeginFramePass(bool clear);
    // How a texture pass starts.  Selecting a target clears it to transparent
    // black (colour and depth); Rnd::Clear on a bound target restarts the pass
    // with what the clear asks for and keeps the rest (TexturePassLoad below).
    struct TexturePassLoad {
        bool clearColor = true;
        Hmx::Color color = Hmx::Color(0.0f, 0.0f, 0.0f, 0.0f);
        bool clearDepth = true;
    };
    // The 1x, depthless pass on the frame target that FlushPostProcessingForOverlay
    // opens; BeginFramePass(false) reopens it once the frame is in it.
    void BeginOverlayPass();
    void BeginTexturePass(RndTex* tex);
    void BeginTexturePass(RndTex* tex, const TexturePassLoad& load);
    void EndActivePass();
public:
    // Clear depth buffer for 2D overlay rendering (HUD on top of 3D scene).
    // Ends current pass, restarts with depth cleared but color preserved.
    void ClearDepthForOverlay() override;
    // Flush post-processing NOW, then start a new pass that draws directly
    // to the framebuffer (bypassing post-proc). Used for HUD overlay that
    // should not be affected by bloom/DOF.
    void FlushPostProcessingForOverlay();
    // RB3 (rndshape::kRetailPostChain): grade the world now, at Rnd::EndWorld,
    // as retail NgRnd does, and carry on drawing the rest of the frame (note
    // highway, HUD, menus) over the graded image in the same MSAA + depth
    // frame pass. A no-op for DC3 and when post-processing is off or done.
    void FlushWorldPost();
    void RunDC3SpotBeams();  // DC3: this frame's beams, ahead of its composite
    void CreateDepthTexture(int w, int h);
    void CreateDefaultTextures();
    void WriteSceneUniforms();
    // WriteSceneUniforms calls, and the scene bind groups they created
    // ([GPUCOUNT] sceneBgWrites= / sceneBgCreates=).
    uint64_t SceneBgWrites() const { return mSceneBgWrites; }
    uint64_t SceneBgCreates() const { return mSceneBgCreates; }
    // Slot 3 of the scene bind group as WriteSceneUniforms last chose it
    // (null = the white fallback).
    wgpu::TextureView ProjLightTexView() const { return mProjLightTexView; }
    // Invalidate cached scene uniforms — forces re-upload next frame.
    // Call when light properties are modified externally (e.g. debug UI).
    void InvalidateSceneUniforms() { mLastSceneEnv = nullptr; }
    void MaybeCaptureFrame();
    void MaybeEncodeVideoFrame();
    FrameReadyCallback mFrameReadyCallback = nullptr;
#ifdef HX_IMGUI
    void RenderImGuiOverlay();
#endif
    GpuDevice mGpu;
    PipelineManager mPipelines;

    // Render passes (extracted subsystems)
    ShadowPass mShadowPass;
    PostProcPass mPostProcPass;
    DisplayRamp mDisplayRamp;   // rndshape::DisplayGamma(): the RB3 Xbox 360 display ramp
    DrawRect2D mDrawRect2D;
    Canvas2D mCanvas2D;
    PointTestPass mPointTestPass;

    // Point tests queued this frame, and whether this frame's world end ran.
    std::vector<PointTestPass::Query> mPointTestQueue;
    bool mPointTestsRan = false;
    uint64_t mPointTestSeqBeforeFrame = 0;  // LastSeq() at BeginDrawing
    // Without the after-submit wait: each tested key's last answer, the keys a
    // full PointTestPass could not record last frame, and scratch.
    PointTestStandIns mPointTestStandIns;
    std::vector<const void*> mPointTestUnrecordedKeys;
    std::vector<const void*> mPointTestLateKeys;
    uint64_t mPointTestEndWaits = 0;
    uint64_t mPointTestStandInsDelivered = 0;
    struct PointTester : NativePointTester {
        WgpuRnd* rnd = nullptr;
        bool QueuePointTest(const NativePointTest& t) override { return rnd->QueuePointTest(t); }
        void CancelPointTests(const void* key) override { rnd->CancelPointTests(key); }
    };
    PointTester mPointTester;

    TexBlendPass mTexBlendPass;
    int mTexBlendsComposed = 0;
    struct TexBlendComposer : NativeTexBlendComposer {
        WgpuRnd* rnd = nullptr;
        bool ComposeTexBlend(RndTex* output, RndTex* base, const NativeTexBlendLayer* layers,
                             int count) override {
            return rnd->ComposeTexBlend(output, base, layers, count);
        }
    };
    TexBlendComposer mTexBlendComposer;

    SpotBeamPass mSpotBeamPass;
    std::vector<SpotBeamPass::Draw> mSpotBeamDraws;  // this frame's
    SpotBeamPass::Camera mSpotBeamCam{};
    wgpu::TextureView mSpotBeamFog;
    float mSpotBeamComposite[3] = {};
    int mSpotBeamsDrawn = 0;                         // last frame's count
    struct SpotBeamRenderer : NativeSpotBeamRenderer {
        WgpuRnd* rnd = nullptr;
        bool drop = false;   // MILO_NO_SPOT_BEAMS: accept the beams, draw none
        bool SubmitSpotBeams(const NativeSpotBeamFrame& f, const NativeSpotBeam* b,
                             int count) override {
            if (drop) return rnd->SubmitSpotBeams(f, b, 0);
            return rnd->SubmitSpotBeams(f, b, count);
        }
    };
    SpotBeamRenderer mSpotBeamRenderer;

    ContactShadowPass mContactShadowPass;
    std::vector<ContactShadowPass::Vertex> mContactShadowVerts;  // queued, not drawn
    int mContactShadowsDrawn = 0;
    int mContactShadowsDrawnLast = 0;
    struct ContactShadowRenderer : NativeContactShadowRenderer {
        WgpuRnd* rnd = nullptr;
        bool WantsContactShadows() override { return rnd->WantsContactShadows(); }
        bool QueueContactShadow(const NativeContactShadow& s) override {
            return rnd->QueueContactShadow(s);
        }
    };
    ContactShadowRenderer mContactShadowRenderer;

    // GPU resource initialization tracking
    bool mGpuResourcesReady = false;

    // Per-frame state
    wgpu::CommandEncoder mEncoder;
    wgpu::RenderPassEncoder mPass;
    wgpu::TextureView mFrameView;
    bool mInPass = false;
    bool mPostProcFlushed = false;
    // FlushPostProcessingForOverlay has moved the frame onto the overlay pass:
    // everything drawn since sits only in the frame target. Resuming the MSAA
    // frame pass from here would resolve the MSAA buffer over it on End.
    bool mFrameInOverlay = false;
    RndTex* mActiveTargetTex = nullptr;
    wgpu::TextureFormat mCurrentTargetFormat = wgpu::TextureFormat::Undefined;
    uint32_t mCurrentSampleCount = 1;
    bool mCurrentPassHasDepth = false;
    uint32_t mCurrentTargetWidth = 0;
    uint32_t mCurrentTargetHeight = 0;
    // np-look47 step 2: a texture pass's attachment pixels over the texture's
    // own (LookSettings::texPassScale); 1 when the target is drawn at its size.
    float mTexPassScaleX = 1.0f;
    float mTexPassScaleY = 1.0f;

    // Uniform buffers (staged: one WriteBuffer each per frame, PLAN H-31;
    // FlushUniformRings before the frame Submit)
    void FlushUniformRings();
    void LogGpuCounters();
    int mGpuCountersEvery = 0;  // MILO_GPU_COUNTERS
    UniformRingBuffer mSceneRing;
    UniformRingBuffer mMaterialRing;
    UniformRingBuffer mObjectRing;
    UniformRingBuffer mBoneRing;

    // Bind groups
    BindGroupCache mBindGroupCache;  // mesh-path material/object/bone groups
    BindGroupCache mPostBindGroupCache;  // the post chain + display ramp (PLAN H-66)
    wgpu::BindGroup mSceneBindGroup;
    uint64_t mSceneBgWrites = 0;   // WriteSceneUniforms calls
    uint64_t mSceneBgCreates = 0;  // ... and the scene bind groups they created
    wgpu::TextureView mProjLightTexView;  // gobo texture for projected light (or white fallback)

    // Depth texture
    wgpu::Texture mDepthTex;
    wgpu::TextureView mDepthView;
    wgpu::TextureView mDepthSampleView;  // depth aspect only, for sampling
    int mDepthWidth = 0;
    int mDepthHeight = 0;

    // MSAA render target (4x) — resolves to surface texture
    static constexpr uint32_t kMSAASamples = 4;
    wgpu::Texture mMsaaTex;
    wgpu::TextureView mMsaaView;
    // The 4x colour the post's intermediate is resolved from, for the depth of
    // field's edge rule (np-look51); none when the frame is not multisampled
    // or the target does not match the intermediate's size.
    wgpu::TextureView MsaaSampleView() const {
        return kMSAASamples > 1 && mMsaaTex && (int)mMsaaTex.GetWidth() == mIntermediateWidth &&
                       (int)mMsaaTex.GetHeight() == mIntermediateHeight
                   ? mMsaaView
                   : wgpu::TextureView();
    }
    int mMsaaWidth = 0;
    int mMsaaHeight = 0;

    // Intermediate texture for post-processing
    wgpu::Texture mIntermediateTex;
    wgpu::TextureView mIntermediateView;
    int mIntermediateWidth = 0;
    int mIntermediateHeight = 0;
    bool mFramePassValid = false;
    // FlushWorldPost's graded frame, copied back over the main pass.
    wgpu::Texture mPostOutTex;
    wgpu::TextureView mPostOutView;
    int mPostOutWidth = 0;
    int mPostOutHeight = 0;
    // FlushWorldPost wrote mPostOutTex this frame (retail mPostProcDone).
    bool mWorldPostDone = false;
    // The colour texture the frame pass writes (or MSAA-resolves into): the
    // post-processing intermediate or the frame target. Set by BeginFramePass.
    wgpu::Texture mFramePassColorTex;
    // SavePreBuffer's copy of the world (retail mPreProcessBuffer /
    // mPreProcessTex) and the clamped linear sampler refraction reads it with.
    wgpu::Texture mPreTex;
    wgpu::TextureView mPreView;
    uint32_t mPreWidth = 0;
    uint32_t mPreHeight = 0;
    bool mPreSaved = false;            // mPreTex holds a world (any frame)
    bool mPreSavedThisFrame = false;   // this frame's world end has saved it
    wgpu::Sampler mFrameSampler;

#ifdef __EMSCRIPTEN__
    // On web, the swapchain surface texture may not reliably support
    // LoadOp::Load between render passes. We render to this owned texture
    // instead of mFrameView, then copy to the swapchain at end-of-frame.
    wgpu::Texture mFrameResolvedTex;
    wgpu::TextureView mFrameResolvedView;
    int mFrameResolvedWidth = 0;
    int mFrameResolvedHeight = 0;
#endif

    // Default textures
    wgpu::Texture mWhiteTex;
    wgpu::TextureView mWhiteTexView;
    wgpu::Texture mFlatNormalTex;
    wgpu::TextureView mFlatNormalTexView;
    wgpu::Texture mBlackTex;
    wgpu::TextureView mBlackTexView;
    wgpu::Texture mBlackCubeTex;
    wgpu::TextureView mBlackCubeTexView;
    wgpu::Sampler mDefaultSampler;

    // Scene uniform tracking — re-upload when camera or environment changes
    RndCam* mLastSceneCam = nullptr;
    RndEnviron* mLastSceneEnv = nullptr;
    RndTex* mLastSceneTarget = nullptr;   // render target the scene uniforms were written for
    uint32_t mLastSceneOffset = 0;
    float mLastSceneViewProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float mLastSceneSpecularAA = 0.0f;
    float mLastSceneMaterialLodBias = 0.0f;
    float mLastSceneUiLodBias = 0.0f;
    float mLastSceneUiArtLodBias = 0.0f;
    float mLastSceneCutoutSampleTest = 0.0f;
    float mLastCamPosX = 0.0f; // detect same-pointer position changes
    float mLastCamPosY = 0.0f;
    float mLastCamPosZ = 0.0f;
    // The self-shadow camera the scene block was written for, and its
    // position (PrepShadow moves the one light camera per character), so a
    // receiver never reads another character's map transform.
    RndCam* mLastSelfShadowCam = nullptr;
    float mLastSelfShadowPos[3] = {0.0f, 0.0f, 0.0f};
    // The self-shadow map mSceneBindGroup binds (null: none). A texture pass
    // into that texture must not bind the group (BeginTexturePass).
    RndTex* mSceneGroupSelfShadowTex = nullptr;

    // Auto-screenshot capture (env-var controlled)
    std::string mScreenshotDir;
    std::vector<int> mCaptureFrames;
    int mCaptureIndex = 0;

    // Per-frame timing log (MILO_FRAME_TIMES=<path>): one CSV row per frame,
    // written at the end of EndDrawing. `period` and `cpu` run from one
    // EndDrawing to the next (the whole frame: game poll, draw and submit);
    // `draw` from BeginDrawing; `pt_wait` is the time spent collecting point
    // test answers; `pt_age` is how many frames after its tests were recorded
    // the oldest answer delivered this frame arrived; `pt_end_wait` and
    // `pt_after_submit` are the time and answers of FinishPointTestReadback.
    // `pipe_ms`, `tex_ms`, `mesh_ms`: time this frame spent creating render
    // pipelines, creating bitmap textures and uploading meshes
    // (gfx/FrameCost.h), all threads. Then, each as a count and ms: `bg`
    // bind-group creates, `buf` buffer creates, `gtex` raw texture creates,
    // `view` texture-view creates (the last three Linux desktop only:
    // gfx/WgpuCallCount.cpp; -1 where not counted), `file` file opens and
    // reads; `acquire_ms` / `present_ms` the surface's; `game_ms` /
    // `game_cpu_ms` the wall and thread-CPU time from the previous
    // EndDrawing to this BeginDrawing (the game's poll, outside the
    // renderer); `scene_bg_writes` / `scene_bg_creates` WriteSceneUniforms
    // calls and the scene bind groups they made (docs section 40);
    // `gpu_wait_ms` the headless frames-in-flight wait for the GPU, `pace_ms`
    // the consumer's frame pacer's sleep (in the game window); `game_vcs` /
    // `game_ivcs` / `draw_vcs` / `draw_ivcs` the thread's voluntary and
    // involuntary context switches in the game window and in the draw (Linux;
    // -1 elsewhere): an involuntary switch is the box taking the CPU away;
    // `end_s` this EndDrawing on the steady clock in seconds (CLOCK_MONOTONIC
    // on Linux), so samples taken with `perf record -k CLOCK_MONOTONIC` can be put in
    // their frame (the row's draw ran from end_s - draw_ms to end_s).
    void WriteFrameTimes();
    // The column-name line, written once when the log opens (and "[FT] "
    // prefixed like the rows on stdout).
    void WriteFrameTimesHeader();
    FILE* mFrameTimes = nullptr;
    bool mFrameTimesStdout = false;  // MILO_FRAME_TIMES=- : rows on stdout, "[FT] " prefixed
    double mFrameTimesBegin = 0.0;    // wall s, BeginDrawing
    double mFrameTimesBeginCpu = 0.0; // thread CPU s, BeginDrawing
    double mFrameTimesLastEnd = 0.0;  // wall s, previous EndDrawing
    double mFrameTimesLastCpu = 0.0;  // thread CPU s, previous EndDrawing
    long mFrameTimesBeginCs[2] = {}, mFrameTimesLastCs[2] = {};  // voluntary, involuntary switches
    uint64_t mFrameTimesLastCostNs[framecost::kKinds] = {}; // framecost::Nanos at the previous row
    uint64_t mFrameTimesLastCount[framecost::kKinds] = {};  // framecost::Count at the previous row
    uint64_t mFrameTimesLastSceneWrites = 0, mFrameTimesLastSceneCreates = 0;
    double mPointTestWaitMs = 0.0;
    double mPointTestEndWaitMs = 0.0;  // in FinishPointTestReadback
    int mPointTestAfterSubmit = 0;     // answers it delivered
    int mPointTestsRecorded = 0;
    int mPointTestAnswers = 0;
    int mPointTestAgeMax = 0;
    int mPointTestStandInsFrame = 0;  // stand-ins delivered this frame (pt_standin)
    struct PointTestBatchFrame {
        uint64_t seq = 0;
        int frame = 0;
    };
    PointTestBatchFrame mPointTestBatchFrames[8];

    // Frame budget tracking (MILO_PERF env var)
    bool mPerfEnabled = false;
    double mFrameStartTime = 0.0;
    double mPerfAccumTime = 0.0;
    int mPerfFrameCount = 0;
    float mPerfMaxFrameMs = 0.0f;
    int mPerfDrawCallAccum = 0;
    int mPerfInvisibleAccum = 0;   // draws not sent (DrawChangesNothing)
    int mPerfBudgetViolations = 0;

    // Video recording (MILO_VIDEO env var)
    VideoEncoder mVideoEncoder;
    uint8_t* mVideoPixels = nullptr;
    size_t mVideoPixelSize = 0;
};

// Global accessor — set during Init
extern WgpuRnd* gWgpuRnd;

// The window opened but nothing can be drawn into it (dc3 PLAN H-33): the
// surface is unusable on every window system GpuDevice tried, or it has never
// handed out a frame. WgpuRnd stops the process with this status rather than
// fall back to an invisible headless run or loop drawing nothing.
constexpr int kNativeSurfaceExitCode = 97;
// Called with a one-line reason and kNativeSurfaceExitCode, after the FATAL
// message is printed; a consumer installs one to route the exit through its own
// exit records. Must not return. Unset: stdio is flushed and exit() runs.
using NativeRenderFatalFn = void (*)(const char* reason, int code);
void SetNativeRenderFatalHandler(NativeRenderFatalFn fn);

// Copy a render target's colour texture back to the CPU: w*h texels, 4 bytes
// each, in the target's own channel order (ChooseRenderTargetFormat). False
// when the texture has no GPU render target. Synchronous (Tex_Wgpu.cpp);
// MILO_DUMP_RT and consumers' tests use it.
bool ReadbackGpuRenderTarget(RndTex* tex, std::vector<uint8_t>& texels, int& w, int& h);
// Feature mark for consumers' tests: this engine clears a bound render target
// on Rnd::Clear (WgpuRnd::Clear) and can read one back.
#define MILO_READBACK_GPU_RENDER_TARGET 1

// Free function wrapper — callable from engine code via extern declaration.
// Flushes PostProc to framebuffer, then starts a new pass for HUD overlay.
void FlushPostProcessingForOverlay();
