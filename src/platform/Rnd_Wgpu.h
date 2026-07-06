// DC3 Native Port — WebGPU Renderer Header
// WgpuRnd (extends NgRnd) and WgpuShaderMgr (extends RndShaderMgr)

#pragma once

#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "gfx/ShadowPass.h"
#include "gfx/PostProcPass.h"
#include "gfx/DrawRect2D.h"
#include "gfx/UniformStructs.h"
#include "gfx/UniformRingBuffer.h"
#include "rndobj/Rnd_NG.h"
#include "rndobj/ShaderMgr.h"

#include "gfx/VideoEncoder.h"

#include <string>
#include <vector>
#include <webgpu/webgpu_cpp.h>

// UniformRingBuffer moved to gfx/UniformRingBuffer.h (W1.5, shared gfx-core TU).

// ============================================================================
// WgpuShaderMgr — captures SetVConstant/SetPConstant into staging area
// ============================================================================

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

// ============================================================================
// WgpuRnd — WebGPU renderer extending NgRnd
// ============================================================================

class WgpuRnd : public NgRnd {
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

private:
    void ApplyViewport();
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
    DrawRect2D mDrawRect2D;

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
    uint32_t mLastSceneOffset = 0;
    float mLastCamPosX = 0.0f; // detect same-pointer position changes
    float mLastCamPosY = 0.0f;
    float mLastCamPosZ = 0.0f;

    // Auto-screenshot capture (env-var controlled)
    std::string mScreenshotDir;
    std::vector<int> mCaptureFrames;
    int mCaptureIndex = 0;

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
