// DC3 Native Port — WebGPU Renderer Header
// WgpuRnd (extends the shape's renderer base: NgRnd on DC3-shaped rndobj) and,
// where the shape has the NG shader layer, WgpuShaderMgr (extends RndShaderMgr).

#pragma once

#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "gfx/ShadowPass.h"
#include "gfx/PostProcPass.h"
#include "gfx/DisplayRamp.h"
#include "gfx/DrawRect2D.h"
#include "gfx/PointTestPass.h"
#include "gfx/SpotBeamPass.h"
#include "gfx/TexBlendPass.h"
#include "gfx/UniformStructs.h"
#include "gfx/UniformRingBuffer.h"
#include "platform/rndshape/RndShape.h"
#include "platform/PointTestHook.h"
#include "platform/SpotBeamHook.h"
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
    void SetVConstant(VShaderConstant, const Vector4&) override {}
    void SetVConstant(VShaderConstant, const float*, unsigned int) override {}
    void SetVConstant(VShaderConstant, int) override {}
    void SetVConstant(VShaderConstant, bool) override {}
    void SetPConstant(PShaderConstant, const Hmx::Matrix4&) override {}
    void SetPConstant(PShaderConstant, RndCubeTex*) override {}
    void SetPConstant(PShaderConstant, const Vector4&) override {}
    void SetPConstant(PShaderConstant, RndTex*) override {}
    void SetPConstant(PShaderConstant, int) override {}
    void SetPConstant(PShaderConstant, bool) override {}
    void SetPConstant4x3(PShaderConstant, const Hmx::Matrix4&) override {}

protected:
    RndShaderProgram* NewShaderProgram() override { return nullptr; }
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
    // Base Rnd override
    void DrawRect(const Hmx::Rect& r, const Hmx::Color& c, RndMat* m,
                  const Hmx::Color* tr, const Hmx::Color* bl) override {
        DrawRect(r, m, kStandardShader, c, tr, bl);
    }

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
    PipelineManager& Pipelines() { return mPipelines; }
    wgpu::RenderPassEncoder& CurrentPass() { return mPass; }
    bool IsInPass() const { return mInPass; }
    wgpu::TextureFormat CurrentTargetFormat() const { return mCurrentTargetFormat; }
    uint32_t CurrentSampleCount() const { return mCurrentSampleCount; }
    bool CurrentPassHasDepth() const { return mCurrentPassHasDepth; }
    uint32_t CurrentTargetWidth() const { return mCurrentTargetWidth; }
    uint32_t CurrentTargetHeight() const { return mCurrentTargetHeight; }
    int FrameID() const { return mFrameID; }

    // Scene bind group (group 0) — updated when camera changes
    wgpu::BindGroup& SceneBindGroup() { return mSceneBindGroup; }
    wgpu::Buffer& SceneBuffer() { return mSceneRing.Buffer(); }
    uint32_t SceneOffset() const { return mLastSceneOffset; }
    // SceneUniforms.viewProj as last written (column-major), for the draw log.
    const float* LastSceneViewProj() const { return mLastSceneViewProj; }
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

    // Create material bind group (group 1)
    wgpu::BindGroup CreateMaterialBindGroup(
        uint32_t bufferOffset, uint32_t bufferSize,
        const MaterialTexViews& texViews,
        wgpu::Sampler& diffuseSampler, wgpu::Sampler& mapSampler);

    // Create object bind group (group 2)
    wgpu::BindGroup CreateObjectBindGroup(uint32_t bufferOffset, uint32_t bufferSize);

    // Create bone bind group (group 3) — for skinned meshes
    wgpu::BindGroup CreateBoneBindGroup(uint32_t bufferOffset, uint32_t bufferSize);

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
    // Counts an answer for the frame-times log (MILO_FRAME_TIMES).
    void NotePointTestAnswer(const PointTestPass::Answer& answer);

    // RndTexBlender composition (platform/TexBlendHook.h): retail
    // RndTexBlender::DrawShowing's base rect and unwrapped controller meshes,
    // recorded into the frame's encoder as one pass over the output texture.
    // A frame pass open on entry is resumed (loaded) afterwards.
    bool ComposeTexBlend(RndTex* output, RndTex* base, const NativeTexBlendLayer* layers,
                         int count);
    int TexBlendsComposed() const { return mTexBlendsComposed; }

    // Volumetric spotlight beams (platform/SpotBeamHook.h, gfx/SpotBeamPass.h):
    // retail NgSpotlightDrawer::DoPost. SubmitSpotBeams keeps this frame's
    // beams, with the camera current at submit; FlushWorldPost draws them
    // against the world's depth and hands the blurred result to the RB3
    // composite. Beams not drawn by the frame's end are dropped.
    bool SubmitSpotBeams(const NativeSpotBeamFrame& frame, const NativeSpotBeam* beams, int count);
    int SpotBeamsDrawn() const { return mSpotBeamsDrawn; }

private:
    void ApplyViewport();
    // The viewport a camera select sets on DC3 (RndCam::Select) and on the Wii
    // (WiiCam::Select): mScreenRect over the current target, depth mZRange.
    // Called by WgpuRnd only for shapes whose RndCam::Select does not do it
    // (rndshape::kCamSelectSetsViewport).
    void ApplyCameraViewport(RndCam* cam);
    void BeginFramePass(bool clear);
    void BeginTexturePass(RndTex* tex);
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
    void CreateDepthTexture(int w, int h);
    void CreateDefaultTextures();
    void WriteSceneUniforms();
    // Invalidate cached scene uniforms — forces re-upload next frame.
    // Call when light properties are modified externally (e.g. debug UI).
    void InvalidateSceneUniforms() { mLastSceneEnv = nullptr; }
    void MaybeCaptureFrame();
    void MaybeEncodeVideoFrame();
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
    PointTestPass mPointTestPass;

    // Point tests queued this frame, and whether this frame's world end ran.
    std::vector<PointTestPass::Query> mPointTestQueue;
    bool mPointTestsRan = false;
    uint64_t mPointTestSeqBeforeFrame = 0;  // LastSeq() at BeginDrawing
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

    // GPU resource initialization tracking
    bool mGpuResourcesReady = false;

    // Per-frame state
    wgpu::CommandEncoder mEncoder;
    wgpu::RenderPassEncoder mPass;
    wgpu::TextureView mFrameView;
    bool mInPass = false;
    bool mPostProcFlushed = false;
    RndTex* mActiveTargetTex = nullptr;
    wgpu::TextureFormat mCurrentTargetFormat = wgpu::TextureFormat::Undefined;
    uint32_t mCurrentSampleCount = 1;
    bool mCurrentPassHasDepth = false;
    uint32_t mCurrentTargetWidth = 0;
    uint32_t mCurrentTargetHeight = 0;

    // Uniform buffers
    UniformRingBuffer mSceneRing;
    UniformRingBuffer mMaterialRing;
    UniformRingBuffer mObjectRing;
    UniformRingBuffer mBoneRing;

    // Bind groups
    wgpu::BindGroup mSceneBindGroup;
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
    float mLastCamPosX = 0.0f; // detect same-pointer position changes
    float mLastCamPosY = 0.0f;
    float mLastCamPosZ = 0.0f;

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
    void WriteFrameTimes();
    FILE* mFrameTimes = nullptr;
    double mFrameTimesBegin = 0.0;    // wall s, BeginDrawing
    double mFrameTimesLastEnd = 0.0;  // wall s, previous EndDrawing
    double mFrameTimesLastCpu = 0.0;  // thread CPU s, previous EndDrawing
    double mPointTestWaitMs = 0.0;
    double mPointTestEndWaitMs = 0.0;  // in FinishPointTestReadback
    int mPointTestAfterSubmit = 0;     // answers it delivered
    int mPointTestsRecorded = 0;
    int mPointTestAnswers = 0;
    int mPointTestAgeMax = 0;
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
    int mPerfBudgetViolations = 0;

    // Video recording (MILO_VIDEO env var)
    VideoEncoder mVideoEncoder;
    uint8_t* mVideoPixels = nullptr;
    size_t mVideoPixelSize = 0;
};

// Global accessor — set during Init
extern WgpuRnd* gWgpuRnd;

// Free function wrapper — callable from engine code via extern declaration.
// Flushes PostProc to framebuffer, then starts a new pass for HUD overlay.
void FlushPostProcessingForOverlay();
