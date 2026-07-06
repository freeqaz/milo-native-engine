// DC3 Native Port — WebGPU Renderer Implementation
// Replaces Rnd_Stub.cpp with real WebGPU rendering via Dawn

#include "platform/Rnd_Wgpu.h"
#include "platform/MeshGpuCache.h"
#include "platform/NativeSettings.h"
#include "platform/TexGpu.h"
#include "platform/TransparentQueue.h"

#include "gfx/FrameCapture.h"
#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "gfx/Screenshot.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/Mat.h"
#include "rndobj/PostProc.h"
#include "rndobj/ColorXfm.h"
#include "rndobj/Dir.h"
#include "rndobj/HiResScreen.h"
#include "rndobj/Mesh.h"
#include "rndobj/Utl.h"
#include "gfx/VertexFormats.h"
#include "obj/Dir.h"
#include "obj/DirLoader.h"
#include "obj/Utl.h"
#include "ui/UI.h"
// Phase 0.2a: game-specific draw passes (HamDirector overlay, HamCharacter
// impostor RTT loop) are dispatched through GameRenderHook so the renderer
// stays engine-clean. DC3 registers a HamRenderHook in dc3_render_hook.cpp.
#include "platform/GameRenderHook.h"
#include "world/Dir.h"
#include "char/Character.h"
#include "math/Utl.h"

#ifdef HX_IMGUI
#include <imgui.h>
#include <imgui_impl_wgpu.h>
#include "platform/DebugPanel.h"
#endif

#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#else
#include <emscripten/html5.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

// ============================================================================
// Global instances
// ============================================================================

static WgpuShaderMgr gWgpuShaderMgr;
static WgpuRnd gWgpuRndInstance;

static double PerfNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Rnd& TheRnd = gWgpuRndInstance;
NgRnd& TheNgRnd = gWgpuRndInstance;
RndShaderMgr& TheShaderMgr = gWgpuShaderMgr;
WgpuRnd* gWgpuRnd = &gWgpuRndInstance;

// Exposed for input subsystem (Joypad_Native, Keyboard_Native)
#ifndef __EMSCRIPTEN__
GLFWwindow *gNativeWindow = nullptr;
#endif

UIManager* TheUI = nullptr;

void WgpuShaderMgr::Init() {
    if (mPreInitialized) {
        return;
    }

    mUseAO = 0;
    mPreInitialized = true;
    mBoneCount = 0;
    unk14 = 1;
    mInDepthVolume = 0;
    unk1c = 0;
    mCullModeOverride = 0;
    unk24 = 0;
    unk25 = 0;
    unk26 = 0;
    unk27 = 0;
    unk28 = 0;
    unk29 = 0;
    unk2a = 0;
    unk2b = 0;
    unk2c = 0;
    unk2d = 0;
    unk2e = 0;
    unk2f = 0;
    unk30 = 0;
    unk31 = 0;
    unk34 = 0;
    unk38 = 0;
    unk39 = 0;
    unk3a = 0;
    unk3b = 0;
    unk3c = 0;
    unk3d = 0;
    unk3e = 0;
    unk3f = 0;
    mAllowPerPixel = 1;
    unk41 = 1;
    mDisplayShaderError = true;
    mShaderSize = 0x38;

    RELEASE(mWorkMat);
    RELEASE(mPostProcMat);
    RELEASE(mDrawHighlightMat);
    RELEASE(mDrawRectMat);
    mWorkMat = Hmx::Object::New<RndMat>();
    mPostProcMat = Hmx::Object::New<RndMat>();
    mDrawHighlightMat = Hmx::Object::New<RndMat>();
    mDrawRectMat = Hmx::Object::New<RndMat>();
    CreateAndSetMetaMat(mWorkMat);
    CreateAndSetMetaMat(mPostProcMat);
    mDrawHighlightMat->SetUseEnv(false);
    mDrawHighlightMat->SetZMode(kZModeForce);
    mDrawHighlightMat->SetBlend(BaseMaterial::kBlendSrc);
    mDrawHighlightMat->SetAlphaCut(false);
    CreateAndSetMetaMat(mDrawHighlightMat);
    mDrawRectMat->SetZMode(kZModeDisable);
    mDrawRectMat->SetUseEnv(false);
    mDrawRectMat->SetPreLit(true);
    mDrawRectMat->SetBlend(BaseMaterial::kBlendSrcAlpha);
    mDrawRectMat->SetAlphaCut(false);
    CreateAndSetMetaMat(mDrawRectMat);

    MILO_ASSERT(mConstantCache == NULL, 0);
    mConstantCacheSize = 516;
    {
        MemTemp tmp;
        mConstantCache = new float[mConstantCacheSize];
    }
}

void WgpuShaderMgr::Terminate() {
    RELEASE(mDrawHighlightMat);
    RELEASE(mDrawRectMat);
    RELEASE(mWorkMat);
    RELEASE(mPostProcMat);
    RndShaderMgr::Terminate();
}

// UniformRingBuffer method bodies moved to gfx/UniformRingBuffer.cpp (W1.5).

// ============================================================================
// Helper: Convert Milo Transform to 16-float row-major 4x4 matrix
// (WGSL interprets as column-major, giving the correct transpose for M*v)
// ============================================================================

static void TransformToFloat16(const Transform& xfm, float* out) {
    out[0]  = xfm.m.x.x; out[1]  = xfm.m.x.y; out[2]  = xfm.m.x.z; out[3]  = 0;
    out[4]  = xfm.m.y.x; out[5]  = xfm.m.y.y; out[6]  = xfm.m.y.z; out[7]  = 0;
    out[8]  = xfm.m.z.x; out[9]  = xfm.m.z.y; out[10] = xfm.m.z.z; out[11] = 0;
    out[12] = xfm.v.x;   out[13] = xfm.v.y;   out[14] = xfm.v.z;   out[15] = 1;
}

static NgRnd::Viewport BuildViewportForScreenRect(
    int width, int height, const Hmx::Rect& screenRect, float minZ, float maxZ
) {
    Hmx::Rect r;
    if (TheHiResScreen.IsActive()) {
        Hmx::Rect tileRect;
        TheHiResScreen.CurrentTileRect(screenRect, r, tileRect);
    } else {
        float x = screenRect.x;
        float y = screenRect.y;
        float x2 = screenRect.w + x;
        float y2 = screenRect.h + y;
        r.x = Max(0.0f, x);
        r.y = Max(0.0f, y);
        x2 = Max(0.0f, x2);
        y2 = Max(0.0f, y2);
        r.x = Min(1.0f, r.x);
        r.y = Min(1.0f, r.y);
        x2 = Min(1.0f, x2);
        y2 = Min(1.0f, y2);
        r.w = x2 - r.x;
        r.h = y2 - r.y;
    }

    NgRnd::Viewport vp;
    vp.X = (unsigned int)((float)width * r.x);
    vp.Y = (unsigned int)((float)height * r.y);
    vp.Width = (unsigned int)((float)width * r.w);
    vp.Height = (unsigned int)((float)height * r.h);
    vp.MinZ = minZ;
    vp.MaxZ = maxZ;
    return vp;
}

// ============================================================================
// WgpuRnd Implementation
// ============================================================================

void WgpuRnd::Init() {
    printf("DC3 Native: WgpuRnd::Init() — WebGPU renderer\n");

    // Register subsystem types (creates default cam/env/mat/etc.)
    PreInit();
    TheShaderMgr.Init();

    // Override clear color — DTA config isn't loaded in the native port.
    // Default to medium-dark teal to approximate the turbo_shell venue.
    // Without venue geometry, this is the only background; brighter values
    // make the multiply-blend overlays visible and SrcAlpha overlays less jarish.
    // MILO_CLEAR_COLOR=r,g,b overrides (e.g. "1,1,1" for white, "0.5,0.5,0.5" for gray)
    const char* clearEnv = getenv("MILO_CLEAR_COLOR");
    if (clearEnv) {
        float r = 0, g = 0, b = 0;
        sscanf(clearEnv, "%f,%f,%f", &r, &g, &b);
        mClearColor = Hmx::Color(r, g, b);
    } else {
        mClearColor = Hmx::Color(0.06f, 0.09f, 0.12f);
    }

    // Initialize camera/rendering tuning from env vars
    NativeSettings::Get().Init();

    // Create GPU device and window
    GpuDeviceDesc desc{};
#ifdef __EMSCRIPTEN__
    // On web: always init GPU (canvas surface, async adapter/device request).
    // InitGpuResources() must be called after mGpu.IsReady().
    desc.headless = false;
    desc.width = 1280;
    desc.height = 720;
    mGpu.Init(desc);
    printf("DC3 Web: WgpuRnd::Init() — GPU init started (async)\n");
#else
    // GPU rendering is enabled by default. Set MILO_NORENDER=1 to disable.
    // Legacy MILO_RENDER=1 still works for backwards compat.
    bool enableGpu = !getenv("MILO_NORENDER");
    if (getenv("MILO_RENDER")) enableGpu = true;  // explicit override
    if (enableGpu) {
        desc.headless = (getenv("MILO_HEADLESS") != nullptr);
        desc.width = getenv("MILO_WIDTH") ? atoi(getenv("MILO_WIDTH")) : 1280;
        desc.height = getenv("MILO_HEIGHT") ? atoi(getenv("MILO_HEIGHT")) : 720;
        desc.title = "DC3 Native — WebGPU";

        if (!mGpu.Init(desc)) {
            printf("DC3 Native: GPU init failed, falling back to headless\n");
            desc.headless = true;
            if (!mGpu.Init(desc)) {
                printf("DC3 Native: headless GPU init also failed!\n");
                return;
            }
        }

        gNativeWindow = mGpu.Window();
        InitGpuResources();
    } else {
        printf("DC3 Native: GPU init skipped (set MILO_RENDER=1 or unset MILO_NORENDER to enable)\n");
    }
#endif
    mPerfEnabled = (getenv("MILO_PERF") != nullptr);
    if (mPerfEnabled) {
        printf("DC3 Native: frame budget tracking enabled (MILO_PERF)\n");
    }
}

void WgpuRnd::InitGpuResources() {
    if (mGpuResourcesReady) return; // idempotent — already initialized
    mGpuResourcesReady = true;

    // Initialize pipeline manager
    mPipelines.Init(&mGpu);
    // Create per-draw ring buffers (64KB each — enough for ~250 draws/frame at 256-byte alignment)
    // Scene ring handles mid-frame camera switches (each camera gets its own offset)
    mSceneRing.Init(mGpu.Device(), 16 * 1024, "SceneUniforms");
    mMaterialRing.Init(mGpu.Device(), 64 * 1024, "MaterialUniforms");
    mObjectRing.Init(mGpu.Device(), 64 * 1024, "ObjectUniforms");
    // Bone ring needs more space: 2560 bytes per skinned draw (rounded to 2816 at 256 alignment)
    mBoneRing.Init(mGpu.Device(), 256 * 1024, "BoneUniforms");

    // Create depth texture (use GPU framebuffer dimensions, not Rnd virtual resolution)
    CreateDepthTexture(mGpu.WindowWidth(), mGpu.WindowHeight());

    // Create default textures (1x1 white for untextured materials)
    CreateDefaultTextures();

    // Initialize render passes
    mShadowPass.Init(mGpu);
    mPostProcPass.Init(mGpu);

    // Native port: disable Xbox 360 safe area shrink (TVs need overscan
    // compensation, but PC/Mac monitors don't).
    SetShrinkToSafeArea(false);

    printf("WgpuRnd: GPU resources initialized (%dx%d, Rnd %dx%d, %s)\n",
           mGpu.WindowWidth(), mGpu.WindowHeight(), mWidth, mHeight,
           mGpu.HasBCCompression() ? "BC supported" : "software DXT");

#ifndef __EMSCRIPTEN__
    // Frame capture setup (env-var controlled, native only)
    const char* captureFrame = getenv("MILO_CAPTURE_FRAME");
    if (captureFrame && captureFrame[0]) {
        int frame = atoi(captureFrame);
        if (frame > 0) {
            FrameCapture::Get().SetCaptureFrame(frame);
            printf("DC3 Native: frame capture armed for frame %d\n", frame);
        }
    }

    // Auto-screenshot setup (env-var controlled, native only)
    const char* ssDir = getenv("MILO_SCREENSHOT_DIR");
    if (ssDir && ssDir[0]) {
        mScreenshotDir = ssDir;
        const char* ssFrames = getenv("MILO_SCREENSHOT_FRAMES");
        if (!ssFrames || !ssFrames[0]) ssFrames = "100,600,900,1500";
        std::istringstream iss(ssFrames);
        std::string token;
        while (std::getline(iss, token, ',')) {
            int frame = atoi(token.c_str());
            if (frame > 0) mCaptureFrames.push_back(frame);
        }
        mCaptureIndex = 0;
        printf("DC3 Native: auto-screenshot enabled — dir=%s frames=", mScreenshotDir.c_str());
        for (size_t i = 0; i < mCaptureFrames.size(); i++) {
            if (i > 0) printf(",");
            printf("%d", mCaptureFrames[i]);
        }
        printf("\n");
    }

    // Video recording setup (env-var controlled, native only)
    const char* videoPath = getenv("MILO_VIDEO");
    if (videoPath && videoPath[0]) {
        int w = mGpu.WindowWidth();
        int h = mGpu.WindowHeight();
        int fps = 30;
        const char* fpsEnv = getenv("MILO_VIDEO_FPS");
        if (fpsEnv && fpsEnv[0]) fps = atoi(fpsEnv);
        if (fps <= 0) fps = 30;

        mVideoPixelSize = (size_t)w * h * 4;
        mVideoPixels = (uint8_t*)malloc(mVideoPixelSize);
        if (mVideoPixels) {
            mVideoEncoder.Start(videoPath, w, h, fps);
        }
    }
#endif
}

void WgpuRnd::Terminate() {
    extern void BoneSetupTerminate();
    extern void PartTerminate();

    // Finalize video recording before GPU teardown
    mVideoEncoder.Finish();
    if (mVideoPixels) {
        free(mVideoPixels);
        mVideoPixels = nullptr;
    }

#ifndef __EMSCRIPTEN__
    gNativeWindow = nullptr;
#endif

    // Release all GPU objects BEFORE device shutdown to avoid
    // use-after-free in static destructor (Dawn/Vulkan teardown ordering)
    mDepthTex = nullptr;
    mDepthView = nullptr;
    mWhiteTex = nullptr;
    mWhiteTexView = nullptr;
    mFlatNormalTex = nullptr;
    mFlatNormalTexView = nullptr;
    mBlackTex = nullptr;
    mBlackTexView = nullptr;
    mBlackCubeTex = nullptr;
    mBlackCubeTexView = nullptr;
    mDefaultSampler = nullptr;
    mSceneRing.Release();
    mSceneBindGroup = nullptr;
    mProjLightTexView = nullptr;
    mMsaaTex = nullptr;
    mMsaaView = nullptr;

    // Ring buffers
    mMaterialRing.Release();
    mObjectRing.Release();
    mBoneRing.Release();

    // PipelineManager
    mPipelines.Terminate();

    // Render passes
    mDrawRect2D.Terminate();
    mPostProcPass.Terminate();
    mShadowPass.Terminate();

    BoneSetupTerminate();
    PartTerminate();

    // Intermediate texture
    mIntermediateTex = nullptr;
    mIntermediateView = nullptr;

#ifdef __EMSCRIPTEN__
    mFrameResolvedTex = nullptr;
    mFrameResolvedView = nullptr;
#endif

    // Per-frame state
    mEncoder = nullptr;
    mPass = nullptr;
    mFrameView = nullptr;

    mGpu.Shutdown();
}

void WgpuRnd::Clear(unsigned int flags, const Hmx::Color& color) {
    mClearColor = color;
}

void WgpuRnd::EndActivePass() {
    if (!mInPass) {
        return;
    }
    FlushTextDraws();
    FlushTransparentDraws();
    mPass.End();
    mInPass = false;
}

void WgpuRnd::ClearDepthForOverlay() {
    if (!mInPass || !mFrameView) return;
    // End current pass, restart with depth cleared but color preserved
    EndActivePass();
    BeginFramePass(false); // false = Load color (preserve), but we override depth below
    // BeginFramePass(false) uses LoadOp::Load for depth too, which is wrong.
    // We need to restart with cleared depth. End and manually build the pass.
    EndActivePass();

    int curW = mGpu.WindowWidth();
    int curH = mGpu.WindowHeight();
    bool hasPostProc = RndPostProc::Current() != nullptr;

    wgpu::RenderPassColorAttachment colorAtt{};
    if (kMSAASamples > 1) {
        colorAtt.view = mMsaaView;
        colorAtt.resolveTarget = hasPostProc ? mIntermediateView : FrameTarget();
        colorAtt.storeOp = wgpu::StoreOp::Store;
    } else {
        colorAtt.view = hasPostProc ? mIntermediateView : FrameTarget();
        colorAtt.storeOp = wgpu::StoreOp::Store;
    }
    colorAtt.loadOp = wgpu::LoadOp::Load; // preserve color from venue

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear; // clear depth for overlay
    depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = wgpu::LoadOp::Clear;
    depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "OverlayPass";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;
    rpDesc.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rpDesc);
    mInPass = true;
    mActiveTargetTex = nullptr;
    mCurrentTargetFormat = mGpu.SurfaceFormat();
    mCurrentSampleCount = kMSAASamples;
    mCurrentPassHasDepth = true;
    mCurrentTargetWidth = (uint32_t)curW;
    mCurrentTargetHeight = (uint32_t)curH;
    ApplyViewport();

    // Force re-bind scene uniforms on the new pass — the previous pass
    // had them bound, but the new pass starts without any bind groups.
    mLastSceneCam = nullptr;
}

void WgpuRnd::FlushPostProcessingForOverlay() {
    if (!mInPass || !mFrameView) {
        return;
    }

    // End the current pass (venue + any existing draws)
    EndActivePass();

    // Run post-processing now (reads intermediate, writes to framebuffer).
    // Only run once per frame — subsequent calls just start a new overlay pass
    // without re-running post-proc (which would overwrite previous HUD draws).
    if (mIntermediateView && RndPostProc::Current() && !mPostProcFlushed) {
        mPostProcPass.Run(mEncoder, mIntermediateView, mIntermediateTex,
                          mIntermediateWidth, mIntermediateHeight,
                          mDepthView, FrameTarget(), mBlackTexView, mGpu);
    }

    int curW = mGpu.WindowWidth();
    int curH = mGpu.WindowHeight();

    // Start a new pass that draws directly to the framebuffer (no post-proc).
    // The HUD overlay draws at 1x (no MSAA) directly to the frame target,
    // with no depth buffer. This avoids MSAA sample-count mismatches and
    // preserves the post-processed venue that's already in the framebuffer.
    // On web, FrameTarget() is an owned texture (not the swapchain surface)
    // so LoadOp::Load reliably preserves the post-proc output.
    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = FrameTarget();
    colorAtt.storeOp = wgpu::StoreOp::Store;
    colorAtt.loadOp = wgpu::LoadOp::Load; // preserve post-processed venue

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "HudOverlayPass";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;
    rpDesc.depthStencilAttachment = nullptr; // no depth for 2D HUD

    mPass = mEncoder.BeginRenderPass(&rpDesc);
    mInPass = true;
    mActiveTargetTex = nullptr;
    mCurrentTargetFormat = mGpu.SurfaceFormat();
    mCurrentSampleCount = 1;  // 1x for HUD overlay
    mCurrentPassHasDepth = false;
    mCurrentTargetWidth = (uint32_t)curW;
    mCurrentTargetHeight = (uint32_t)curH;
    ApplyViewport();

    // Force scene uniforms re-bind on the new pass
    mLastSceneCam = nullptr;
    // Mark post-proc as already done so EndDrawing doesn't run it again
    mPostProcFlushed = true;

    // Game-supplied HUD/overlay draw pass (DC3 HamDirector overlay, RB3 band
    // HUD). The overlay pass is now active on the framebuffer at 1x with no
    // depth; the hook issues whatever draws it needs.
    if (GameRenderHook* hook = GetGameRenderHook()) {
        hook->DrawGameOverlay(this);
    }
}

void FlushPostProcessingForOverlay() {
    gWgpuRndInstance.FlushPostProcessingForOverlay();
}

void WgpuRnd::SetViewport(const Viewport& v) {
    NgRnd::SetViewport(v);
    ApplyViewport();
}

void WgpuRnd::ApplyViewport() {
    if (!mInPass) {
        return;
    }
    const Viewport& vp = GetViewport();
    uint32_t width = vp.Width ? vp.Width : mCurrentTargetWidth;
    uint32_t height = vp.Height ? vp.Height : mCurrentTargetHeight;
    mPass.SetViewport((float)vp.X, (float)vp.Y, (float)width, (float)height, vp.MinZ, vp.MaxZ);
}

void WgpuRnd::BeginFramePass(bool clear) {
    if (!mFrameView) {
        return;
    }

    int curW = mGpu.WindowWidth();
    int curH = mGpu.WindowHeight();
    bool hasPostProc = RndPostProc::Current() != nullptr;

    wgpu::RenderPassColorAttachment colorAtt{};
    if (kMSAASamples > 1) {
        colorAtt.view = mMsaaView;
        if (hasPostProc) {
            if (mIntermediateWidth != curW || mIntermediateHeight != curH || !mIntermediateTex) {
                wgpu::TextureDescriptor iDesc{};
                iDesc.label = "PostProcIntermediate";
                iDesc.size = {(uint32_t)curW, (uint32_t)curH, 1};
                iDesc.format = mGpu.SurfaceFormat();
                iDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
                iDesc.mipLevelCount = 1;
                mIntermediateTex = mGpu.Device().CreateTexture(&iDesc);
                mIntermediateView = mIntermediateTex.CreateView();
                mIntermediateWidth = curW;
                mIntermediateHeight = curH;
            }
            colorAtt.resolveTarget = mIntermediateView;
        } else {
            colorAtt.resolveTarget = FrameTarget();
        }
        // The frame pass can be interrupted by offscreen render-to-texture work.
        // Preserve MSAA contents so we can resume the main frame afterward.
        colorAtt.storeOp = wgpu::StoreOp::Store;
    } else {
        if (hasPostProc) {
            if (mIntermediateWidth != curW || mIntermediateHeight != curH || !mIntermediateTex) {
                wgpu::TextureDescriptor iDesc{};
                iDesc.label = "PostProcIntermediate";
                iDesc.size = {(uint32_t)curW, (uint32_t)curH, 1};
                iDesc.format = mGpu.SurfaceFormat();
                iDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
                iDesc.mipLevelCount = 1;
                mIntermediateTex = mGpu.Device().CreateTexture(&iDesc);
                mIntermediateView = mIntermediateTex.CreateView();
                mIntermediateWidth = curW;
                mIntermediateHeight = curH;
            }
            colorAtt.view = mIntermediateView;
        } else {
            colorAtt.view = FrameTarget();
        }
        colorAtt.storeOp = wgpu::StoreOp::Store;
    }
    colorAtt.loadOp = clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    colorAtt.clearValue = {
        (double)mClearColor.red,
        (double)mClearColor.green,
        (double)mClearColor.blue,
        1.0
    };

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mDepthView;
    depthAtt.depthLoadOp = clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;
    depthAtt.stencilLoadOp = clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
    depthAtt.stencilClearValue = 0;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = clear ? "MainPass" : "MainPassResume";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;
    rpDesc.depthStencilAttachment = &depthAtt;

    mPass = mEncoder.BeginRenderPass(&rpDesc);
    mInPass = true;
    mActiveTargetTex = nullptr;
    mFramePassValid = true;
    mCurrentTargetFormat = mGpu.SurfaceFormat();
    mCurrentSampleCount = kMSAASamples;
    mCurrentPassHasDepth = true;
    mCurrentTargetWidth = (uint32_t)curW;
    mCurrentTargetHeight = (uint32_t)curH;
    if (getenv("MILO_DEBUG_PIPELINES")) {
        printf(
            "DC3 Pass: MainPass clear=%d fmt=%d samples=%u depth=%d size=%ux%u\n",
            clear ? 1 : 0,
            (int)mCurrentTargetFormat,
            mCurrentSampleCount,
            mCurrentPassHasDepth ? 1 : 0,
            mCurrentTargetWidth,
            mCurrentTargetHeight
        );
    }
    mPass.SetBindGroup(0, mSceneBindGroup);
    RndCam* cam = RndCam::Current();
    const Viewport& prevVp = GetViewport();
    if (cam && cam->TargetTex() == nullptr) {
        SetViewport(BuildViewportForScreenRect(
            curW, curH, cam->GetScreenRect(), prevVp.MinZ, prevVp.MaxZ
        ));
    } else {
        ApplyViewport();
    }
}

void WgpuRnd::BeginTexturePass(RndTex* tex) {
    if (!tex) return;
    // Ensure the GPU render target exists (lazy creation).
    // GetGpuTexView calls EnsureRenderTargetData for render-target textures,
    // which sets up the texture + depth + renderTarget flag.
    wgpu::TextureView colorView = GetGpuTexView(tex);
    // Only textures with a proper RGBA GPU render target can be used as
    // color attachments. Compressed textures (BC1/BC3) lack RenderAttachment
    // usage and would invalidate the entire command encoder.
    if (!IsGpuTexRenderable(tex)) {
        return;
    }
    if (!colorView) {
        return;
    }

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = colorView;
    colorAtt.loadOp = wgpu::LoadOp::Clear;
    colorAtt.storeOp = wgpu::StoreOp::Store;
    // Use transparent black so impostor/billboard RTT can alpha-cut
    // the background away. Other RTT users (TexRenderer) don't rely on
    // background alpha — they overwrite every pixel.
    colorAtt.clearValue = {0.0, 0.0, 0.0, 0.0};

    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "TexturePass";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;

    wgpu::TextureView depthView = GetGpuTexDepthView(tex);
    if (depthView) {
        depthAtt.view = depthView;
        depthAtt.depthLoadOp = wgpu::LoadOp::Clear;
        depthAtt.depthStoreOp = wgpu::StoreOp::Store;
        depthAtt.depthClearValue = 1.0f;
        depthAtt.stencilLoadOp = wgpu::LoadOp::Clear;
        depthAtt.stencilStoreOp = wgpu::StoreOp::Store;
        depthAtt.stencilClearValue = 0;
        rpDesc.depthStencilAttachment = &depthAtt;
    }

    mPass = mEncoder.BeginRenderPass(&rpDesc);
    mInPass = true;
    mActiveTargetTex = tex;
    mCurrentTargetFormat = ChooseRenderTargetFormat(tex);
    mCurrentSampleCount = 1;
    mCurrentPassHasDepth = depthView != nullptr;
    mCurrentTargetWidth = (uint32_t)tex->Width();
    mCurrentTargetHeight = (uint32_t)tex->Height();
    if (getenv("MILO_DEBUG_PIPELINES")) {
        printf(
            "DC3 Pass: TexturePass tex='%s' fmt=%d samples=%u depth=%d size=%ux%u\n",
            tex->Name(),
            (int)mCurrentTargetFormat,
            mCurrentSampleCount,
            mCurrentPassHasDepth ? 1 : 0,
            mCurrentTargetWidth,
            mCurrentTargetHeight
        );
    }
    mPass.SetBindGroup(0, mSceneBindGroup);
    RndCam* cam = RndCam::Current();
    const Viewport& prevVp = GetViewport();
    if (cam && cam->TargetTex() == tex) {
        SetViewport(BuildViewportForScreenRect(
            tex->Width(), tex->Height(), cam->GetScreenRect(), prevVp.MinZ, prevVp.MaxZ
        ));
    } else {
        Viewport vp;
        vp.X = 0;
        vp.Y = 0;
        vp.Width = (unsigned int)tex->Width();
        vp.Height = (unsigned int)tex->Height();
        vp.MinZ = prevVp.MinZ;
        vp.MaxZ = prevVp.MaxZ;
        SetViewport(vp);
    }
}

void WgpuRnd::SelectRenderTarget(RndTex* tex) {
    if (!mGpu.Device() || !mFrameView) {
        return;
    }
    EndActivePass();
    BeginTexturePass(tex);
}

void WgpuRnd::FinishRenderTarget(RndTex* tex) {
    if (mActiveTargetTex != tex) {
        return;
    }
    EndActivePass();
    mActiveTargetTex = nullptr;
}

void WgpuRnd::MakeDrawTarget() {
    if (!mGpu.Device() || !mFrameView) {
        return;
    }
    if (mActiveTargetTex) {
        EndActivePass();
        mActiveTargetTex = nullptr;
    }
    if (!mInPass) {
        BeginFramePass(false);
    }
}

void WgpuRnd::BeginDrawing() {
    RndMesh_ResetFrameStats();
    mPostProcFlushed = false;

    // Frame capture: set target frame via MILO_CAPTURE_FRAME env var
    {
        static int sCaptureFrame = -1;
        static bool sChecked = false;
        if (!sChecked) {
            sChecked = true;
            const char *env = getenv("MILO_CAPTURE_FRAME");
            if (env) sCaptureFrame = atoi(env);
        }
        FrameCapture::Get().BeginFrame(mFrameID);
        if (sCaptureFrame >= 0 && mFrameID == sCaptureFrame)
            FrameCapture::Get().SetCaptureFrame(sCaptureFrame);
    }

    // Skip if GPU not initialized or device lost
    if (mGpu.IsDeviceLost()) {
        static int sLostLogCount = 0;
        if (sLostLogCount++ < 3)
            fprintf(stderr, "WgpuRnd::BeginDrawing: device lost, skipping frame %d\n", mFrameID);
    }
    if (!mGpu.IsReady()) {
        mDrawing = true;
        mWorldEnded = false;
        mDrawCount++;
        mFrameID++;

        return;
    }
    // Poll GLFW events
    if (!mGpu.IsHeadless()) {
        mGpu.PollEvents();
        if (mGpu.ShouldClose()) {
            // Window closed — request shutdown
            // For now, just skip drawing
            mDrawing = true;
            mWorldEnded = false;
            mDrawCount++;
            mFrameID++;
            return;
        }
    }

    mDrawing = true;
    mWorldEnded = false;
    mDrawCount++;
    mFrameID++;

    if (mPerfEnabled) {
        mFrameStartTime = PerfNow();
    }

    FrameCapture::Get().BeginFrame(mFrameID);

    // F12 triggers capture for next frame
#ifndef __EMSCRIPTEN__
    if (gNativeWindow && !mGpu.IsHeadless()) {
        if (glfwGetKey(gNativeWindow, GLFW_KEY_F12) == GLFW_PRESS)
            FrameCapture::Get().CaptureNextFrame();
    }
#endif

#if defined(HX_IMGUI) && !defined(MILO_VIEWER) && !defined(__EMSCRIPTEN__)
    // Backtick (~) toggles ImGui debug panel
    if (gNativeWindow && !mGpu.IsHeadless()) {
        static bool tildeWasPressed = false;
        bool tildePressed = glfwGetKey(gNativeWindow, GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS;
        if (tildePressed && !tildeWasPressed)
            DebugPanel::Toggle();
        tildeWasPressed = tildePressed;
    }
#endif

    // Select default camera and environment (base Rnd::BeginDrawing does this)
    // Only if no camera is already current (viewer sets its own orbit camera)
    if (mDefaultCam && !RndCam::Current())
        mDefaultCam->Select();
    if (mDefaultEnv && !RndEnviron::Current())
        mDefaultEnv->Select(nullptr);

    // Reset ring buffers for this frame
    mSceneRing.Reset();
    mMaterialRing.Reset();
    mObjectRing.Reset();
    mBoneRing.Reset();

#ifndef __EMSCRIPTEN__
    // Poll window size each frame and reconfigure surface if changed.
    // This catches resize events that the callback may miss (e.g. macOS
    // live resize where the callback fires but Dawn needs reconfiguration
    // before the next AcquireNextFrame).
    if (!mGpu.IsHeadless() && mGpu.Window()) {
        int winW, winH;
        glfwGetWindowSize(mGpu.Window(), &winW, &winH);
        if (winW > 0 && winH > 0 &&
            (winW != mGpu.WindowWidth() || winH != mGpu.WindowHeight())) {
            mGpu.ResizeSurface(winW, winH);
        }
    }
#else
    // Web: poll canvas buffer size and sync GPU surface.
    // The JS ResizeObserver may fire before WASM is ready, leaving the
    // surface at its initial 1280x720 while the canvas is CSS-sized.
    {
        int canvasW, canvasH;
        emscripten_get_canvas_element_size("#dc3-canvas", &canvasW, &canvasH);
        if (canvasW > 0 && canvasH > 0 &&
            (canvasW != mGpu.WindowWidth() || canvasH != mGpu.WindowHeight())) {
            mGpu.ResizeSurface(canvasW, canvasH);
        }
    }
#endif

    // Acquire next frame
    if (mGpu.IsHeadless()) {
        mFrameView = mGpu.AcquireHeadlessFrame();
    } else {
        mFrameView = mGpu.AcquireNextFrame();
    }
    if (!mFrameView) {
#ifdef DEBUG_LOGS
        static int sFailCount = 0;
        if (sFailCount < 10) {
            printf("DC3 Native: BeginDrawing — frame acquisition failed (headless=%d, frame=%d)\n",
                   mGpu.IsHeadless(), mFrameID);
            sFailCount++;
        }
#endif
        return;
    }

    // Resize depth/MSAA textures if window size changed
    int curW = mGpu.WindowWidth();
    int curH = mGpu.WindowHeight();
    if (curW != mDepthWidth || curH != mDepthHeight) {
        CreateDepthTexture(curW, curH);
        // Do NOT update mWidth/mHeight — those are the Rnd virtual resolution for UI layout
    }
    // Ensure MSAA color target exists (format may not be known until first frame)
    if (mMsaaWidth != curW || mMsaaHeight != curH || !mMsaaTex) {
        wgpu::TextureDescriptor desc{};
        desc.label = "MSAA4xColor";
        desc.size.width = curW;
        desc.size.height = curH;
        desc.size.depthOrArrayLayers = 1;
        desc.format = mGpu.SurfaceFormat();
        desc.usage = wgpu::TextureUsage::RenderAttachment;
        desc.mipLevelCount = 1;
        desc.sampleCount = kMSAASamples;
        mMsaaTex = mGpu.Device().CreateTexture(&desc);
        mMsaaView = mMsaaTex.CreateView();
        mMsaaWidth = curW;
        mMsaaHeight = curH;
    }

#ifdef __EMSCRIPTEN__
    // On web, render to an owned texture instead of the swapchain surface.
    // Swapchain surfaces may not preserve content for LoadOp::Load between
    // render passes (browser-dependent). Copy to swapchain at end-of-frame.
    if (mFrameResolvedWidth != curW || mFrameResolvedHeight != curH || !mFrameResolvedTex) {
        wgpu::TextureDescriptor desc{};
        desc.label = "FrameResolved";
        desc.size = {(uint32_t)curW, (uint32_t)curH, 1};
        desc.format = mGpu.SurfaceFormat();
        desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
        desc.mipLevelCount = 1;
        mFrameResolvedTex = mGpu.Device().CreateTexture(&desc);
        mFrameResolvedView = mFrameResolvedTex.CreateView();
        mFrameResolvedWidth = curW;
        mFrameResolvedHeight = curH;
    }
#endif

    // Write scene uniforms from current camera and environment
    WriteSceneUniforms();
    mLastSceneCam = RndCam::Current();
    mLastSceneEnv = RndEnviron::Current();
    // Create command encoder
    wgpu::CommandEncoderDescriptor encDesc{};
    encDesc.label = "FrameEncoder";
    mEncoder = mGpu.Device().CreateCommandEncoder(&encDesc);

    // Shadow pre-pass: render depth from light's perspective
    mShadowPass.Render(mEncoder, mObjectRing, mBoneRing, mGpu);

    // Pre-clear: render-to-texture passes (TexRenderer, TexMovie)
    // Must happen after encoder creation but before main frame pass.
    // TexRenderer::DrawPreClear() calls SelectRenderTarget() which creates
    // temporary texture passes, then FinishDrawTarget() restores state.
    DrawPreClear();

    // Game-supplied off-screen render passes (DC3 character impostor RTTs,
    // RB3-anything-equivalent). The hook iterates its own game objects and
    // opens/closes its own render passes via the renderer API.
    if (GameRenderHook* hook = GetGameRenderHook()) {
        hook->RenderCharacterImpostors(this);
    }

    BeginFramePass(true);
}

void WgpuRnd::EnsureSceneUniformsCurrent() {
    RndCam* cam = RndCam::Current();
    RndEnviron* env = RndEnviron::Current();
    // Check both pointer identity AND camera position — the UI code modifies
    // the same camera object's position each frame before drawing panels.
    // Must check all 3 position components since scene cameras (e.g. turbo_shell.cam)
    // differ from the default UI camera in X/Z as well as Y.
    const Vector3 &camPos = cam ? cam->WorldXfm().v : Vector3(0, 0, 0);
    bool camChanged = (cam != mLastSceneCam || env != mLastSceneEnv
        || camPos.x != mLastCamPosX || camPos.y != mLastCamPosY || camPos.z != mLastCamPosZ);
    if (camChanged) {
        if (HasTransparentDraws() && !IsFlushingTransparentDraws()) {
            FlushTransparentDraws();
        }
        WriteSceneUniforms();
        // Re-bind the new scene bind group on the active render pass
        if (mInPass) {
            mPass.SetBindGroup(0, mSceneBindGroup);
        }
        mLastSceneCam = cam;
        mLastSceneEnv = env;
        mLastCamPosX = camPos.x;
        mLastCamPosY = camPos.y;
        mLastCamPosZ = camPos.z;
    }
}

void WgpuRnd::EndDrawing() {
    FrameCapture::Get().EndFrame();
    if (!mGpu.IsReady()) {
        mDrawing = false;
        return;
    }

    if (mInPass) {
        EndActivePass();

        // Post-processing: if active, read from intermediate and draw to frame target
        // Skip if already flushed (e.g., FlushPostProcessingForOverlay was called)
        if (mIntermediateView && RndPostProc::Current() && !mPostProcFlushed) {
            mPostProcPass.Run(mEncoder, mIntermediateView, mIntermediateTex,
                              mIntermediateWidth, mIntermediateHeight,
                              mDepthView, FrameTarget(), mBlackTexView, mGpu);
        }

#ifdef HX_IMGUI
        // ImGui overlay pass — rendered after post-processing, on top of everything
        RenderImGuiOverlay();
#endif

#ifdef __EMSCRIPTEN__
        // Copy resolved framebuffer to swapchain surface.
        // All rendering went to mFrameResolvedTex (owned) to avoid LoadOp::Load
        // on the swapchain surface, which is unreliable on some web implementations.
        if (mFrameResolvedTex && mGpu.SurfaceTexture()) {
            wgpu::TexelCopyTextureInfo src{};
            src.texture = mFrameResolvedTex;
            wgpu::TexelCopyTextureInfo dst{};
            dst.texture = mGpu.SurfaceTexture();
            wgpu::Extent3D size = {(uint32_t)mGpu.WindowWidth(),
                                   (uint32_t)mGpu.WindowHeight(), 1};
            mEncoder.CopyTextureToTexture(&src, &dst, &size);
        }
#endif

        wgpu::CommandBuffer cmd = mEncoder.Finish();
        mGpu.Queue().Submit(1, &cmd);

        MaybeCaptureFrame();
        MaybeEncodeVideoFrame();
        FrameCapture::Get().EndFrame();

        if (!mGpu.IsHeadless()) {
            mGpu.PresentFrame();
        }

    }

    mFrameView = nullptr;
    mActiveTargetTex = nullptr;
    mFramePassValid = false;
    mCurrentTargetFormat = wgpu::TextureFormat::Undefined;
    mCurrentSampleCount = 1;
    mCurrentPassHasDepth = false;
    mCurrentTargetWidth = 0;
    mCurrentTargetHeight = 0;
    mDrawing = false;

    // Frame budget tracking (MILO_PERF)
    if (mPerfEnabled && mFrameStartTime > 0.0) {
        double now = PerfNow();
        float frameMs = (float)((now - mFrameStartTime) * 1000.0);
        if (frameMs > mPerfMaxFrameMs) mPerfMaxFrameMs = frameMs;
        if (frameMs > 16.67f) mPerfBudgetViolations++;
        mPerfAccumTime += (now - mFrameStartTime);
        mPerfDrawCallAccum += GetMeshDrawCallsThisFrame();
        mPerfFrameCount++;

        // Log every 5 seconds
        if (mPerfAccumTime >= 5.0) {
            float avgMs = (float)(mPerfAccumTime / mPerfFrameCount * 1000.0);
            float fps = (float)(mPerfFrameCount / mPerfAccumTime);
            float avgDraws = (float)mPerfDrawCallAccum / mPerfFrameCount;
            fprintf(stderr, "[PERF] %.1f fps | avg %.2fms | max %.2fms | %d violations (>16.67ms) | %.0f draws/frame\n",
                    fps, avgMs, mPerfMaxFrameMs, mPerfBudgetViolations, avgDraws);
            mPerfAccumTime = 0.0;
            mPerfFrameCount = 0;
            mPerfMaxFrameMs = 0.0f;
            mPerfDrawCallAccum = 0;
            mPerfBudgetViolations = 0;
        }
    }
}

void WgpuRnd::CreateDepthTexture(int w, int h) {
    if (w <= 0 || h <= 0) return;

    // Depth texture (MSAA)
    {
        wgpu::TextureDescriptor desc{};
        desc.label = "DepthStencil";
        desc.size.width = w;
        desc.size.height = h;
        desc.size.depthOrArrayLayers = 1;
        desc.format = wgpu::TextureFormat::Depth24PlusStencil8;
        desc.usage = wgpu::TextureUsage::RenderAttachment;
        desc.mipLevelCount = 1;
        desc.sampleCount = kMSAASamples;

        mDepthTex = mGpu.Device().CreateTexture(&desc);
        mDepthView = mDepthTex.CreateView();
        mDepthWidth = w;
        mDepthHeight = h;
    }

}

void WgpuRnd::CreateDefaultTextures() {
    // 1x1 white texture for untextured materials
    {
        wgpu::TextureDescriptor desc{};
        desc.label = "DefaultWhite";
        desc.size = {1, 1, 1};
        desc.format = ChooseRenderTargetFormat(nullptr);
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        desc.mipLevelCount = 1;
        mWhiteTex = mGpu.Device().CreateTexture(&desc);
        mWhiteTexView = mWhiteTex.CreateView();

        uint8_t white[4] = {255, 255, 255, 255};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mWhiteTex;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        wgpu::Extent3D extent = {1, 1, 1};
        mGpu.Queue().WriteTexture(&dst, white, 4, &layout, &extent);
    }

    // 1x1 flat normal texture (tangent-space up: 128,128,255)
    {
        wgpu::TextureDescriptor desc{};
        desc.label = "DefaultFlatNormal";
        desc.size = {1, 1, 1};
        desc.format = wgpu::TextureFormat::RGBA8Unorm; // linear, not sRGB
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        desc.mipLevelCount = 1;
        mFlatNormalTex = mGpu.Device().CreateTexture(&desc);
        mFlatNormalTexView = mFlatNormalTex.CreateView();

        uint8_t flatNormal[4] = {128, 128, 255, 255};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mFlatNormalTex;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        wgpu::Extent3D extent = {1, 1, 1};
        mGpu.Queue().WriteTexture(&dst, flatNormal, 4, &layout, &extent);
    }

    // 1x1 black texture (no emission)
    {
        wgpu::TextureDescriptor desc{};
        desc.label = "DefaultBlack";
        desc.size = {1, 1, 1};
        desc.format = wgpu::TextureFormat::RGBA8Unorm;
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        desc.mipLevelCount = 1;
        mBlackTex = mGpu.Device().CreateTexture(&desc);
        mBlackTexView = mBlackTex.CreateView();

        uint8_t black[4] = {0, 0, 0, 255};
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mBlackTex;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        wgpu::Extent3D extent = {1, 1, 1};
        mGpu.Queue().WriteTexture(&dst, black, 4, &layout, &extent);
    }

    // 1x1x6 black cube texture (no environment reflection)
    {
        wgpu::TextureDescriptor desc{};
        desc.label = "DefaultBlackCube";
        desc.size = {1, 1, 6};
        desc.dimension = wgpu::TextureDimension::e2D;
        desc.format = wgpu::TextureFormat::RGBA8Unorm;
        desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        desc.mipLevelCount = 1;
        mBlackCubeTex = mGpu.Device().CreateTexture(&desc);

        uint8_t black[4] = {0, 0, 0, 255};
        for (uint32_t face = 0; face < 6; face++) {
            wgpu::TexelCopyTextureInfo dst{};
            dst.texture = mBlackCubeTex;
            dst.origin = {0, 0, face};
            wgpu::TexelCopyBufferLayout layout{};
            layout.bytesPerRow = 4;
            layout.rowsPerImage = 1;
            wgpu::Extent3D extent = {1, 1, 1};
            mGpu.Queue().WriteTexture(&dst, black, 4, &layout, &extent);
        }

        wgpu::TextureViewDescriptor viewDesc{};
        viewDesc.dimension = wgpu::TextureViewDimension::Cube;
        viewDesc.arrayLayerCount = 6;
        mBlackCubeTexView = mBlackCubeTex.CreateView(&viewDesc);
    }

    // Default sampler (linear filtering, repeat)
    {
        SamplerDesc sd{};
        mDefaultSampler = mGpu.GetSampler(sd);
    }
}

void WgpuRnd::WriteSceneUniforms() {
    SceneUniforms scene{};
    memset(&scene, 0, sizeof(scene));

    // Camera
    RndCam* cam = RndCam::Current();
    if (cam) {
        // Guard: skip rewriting scene uniforms if camera has NaN/inf position.
        // CamShot animation can produce NaN during game_screen transition.
        // Keep the previous good scene uniforms instead of poisoning them.
        const Vector3& camP = cam->WorldXfm().v;
        if (camP.x != camP.x || camP.y != camP.y || camP.z != camP.z
            || camP.x > 1e30f || camP.x < -1e30f
            || camP.y > 1e30f || camP.y < -1e30f
            || camP.z > 1e30f || camP.z < -1e30f) {
            return;  // keep previous good uniforms
        }

        // Check if mViewProjMatrix was externally set (milo-viewer does this).
        const Hmx::Matrix4& vp = cam->GetViewProjMatrix();
        bool isIdentity = (vp.x.x == 1 && vp.x.y == 0 && vp.x.z == 0 && vp.x.w == 0 &&
                           vp.y.x == 0 && vp.y.y == 1 && vp.y.z == 0 && vp.y.w == 0 &&
                           vp.z.x == 0 && vp.z.y == 0 && vp.z.z == 1 && vp.z.w == 0 &&
                           vp.w.x == 0 && vp.w.y == 0 && vp.w.z == 0 && vp.w.w == 1);

        if (!isIdentity) {
            // Use externally-set viewProj (milo-viewer orbit cam path)
            memcpy(scene.viewProj, &vp, 64);
        } else {
            // Use engine's GetViewProjectXfms which accounts for mLocalProjectXfm,
            // screen rect, and the Y/Z axis flip (Milo Y-forward → D3D/WebGPU Z-forward).
            // Ensure mInvWorldXfm is up to date (may be zero if camera was never dirty).
            cam->UpdatedWorldXfm();

            Transform viewXfm;
            Hmx::Matrix4 projMtx;
            cam->GetViewProjectXfms(viewXfm, projMtx);

            // Convert Transform (3x4 row-major) to 4x4 view matrix
            float view[16] = {
                viewXfm.m.x.x, viewXfm.m.x.y, viewXfm.m.x.z, 0,
                viewXfm.m.y.x, viewXfm.m.y.y, viewXfm.m.y.z, 0,
                viewXfm.m.z.x, viewXfm.m.z.y, viewXfm.m.z.z, 0,
                viewXfm.v.x,   viewXfm.v.y,   viewXfm.v.z,   1
            };

            // Projection from engine (already handles FOV, aspect, screen rect, axis flip)
            float proj[16] = {
                projMtx.x.x, projMtx.x.y, projMtx.x.z, projMtx.x.w,
                projMtx.y.x, projMtx.y.y, projMtx.y.z, projMtx.y.w,
                projMtx.z.x, projMtx.z.y, projMtx.z.z, projMtx.z.w,
                projMtx.w.x, projMtx.w.y, projMtx.w.z, projMtx.w.w,
            };

            // ViewProj = View * Proj (row-major multiply).
            // WGSL reads this row-major data as column-major mat4x4f, effectively
            // seeing the transpose. The shader computes VP_wgsl * pos, which equals
            // (VP_rowmajor)^T * pos = pos^T * VP_rowmajor in row-vector form — the
            // same result as D3D's mul(pos, VP).
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    float sum = 0;
                    for (int k = 0; k < 4; k++) {
                        sum += view[i * 4 + k] * proj[k * 4 + j];
                    }
                    scene.viewProj[i * 4 + j] = sum;
                }
            }
            memcpy(scene.view, view, sizeof(view));

            // HUD projection: no scaling needed. The gameplay HUD uses a
            // cylindrical 3D layout with meshes at world X=±700 (view
            // distance ~670), giving NDC_x = ±1.88 — beyond the clip
            // volume [-1,1]. On Xbox 360, the Xenos GPU guard-band
            // rasterizes these triangles without clipping; the viewport
            // scissor clips pixels outside the render target, so panels
            // appear partially off-screen at the edges.
            //
            // On native (WebGPU/Vulkan/Metal), desktop GPUs have a
            // similarly large guard band (typically ±4096+ in viewport
            // coords). Triangles with vertices beyond NDC ±1 are
            // rasterized and viewport-clipped, matching Xbox behavior
            // exactly — no projection hack required.
        }

        // Camera position (in world space, before axis flip)
        const Transform& worldXfm = cam->WorldXfm();
        scene.cameraPos[0] = worldXfm.v.x;
        scene.cameraPos[1] = worldXfm.v.y;
        scene.cameraPos[2] = worldXfm.v.z;

        // Camera debug log (every 60 frames ≈ 1/sec)
        if (NativeSettings::Get().cameraDebug && (mFrameID % 60) == 0) {
            fprintf(stderr, "[CAM] frame=%d fov=%.3f(%.1fdeg) near=%.2f far=%.1f "
                    "pos=(%.1f,%.1f,%.1f) aspect=%.3f zRange=(%.3f,%.3f) fovScale=%.3f\n",
                    mFrameID,
                    cam->YFov(), cam->YFov() * 57.2957795f,
                    cam->NearPlane(), cam->FarPlane(),
                    worldXfm.v.x, worldXfm.v.y, worldXfm.v.z,
                    (float)TheRnd.GetAspect(),
                    cam->ZRange().x, cam->ZRange().y,
                    NativeSettings::Get().fovScale);
        }

    } else {
        // Identity viewProj if no camera
        scene.viewProj[0] = scene.viewProj[5] = scene.viewProj[10] = scene.viewProj[15] = 1;
        scene.view[0] = scene.view[5] = scene.view[10] = scene.view[15] = 1;
    }

    // Environment (fog, ambient, lights)
    RndEnviron* env = RndEnviron::Current();
    if (env && env->AmbientFogOwner()) {
        // Ambient color (with minimum floor for visibility)
        const Hmx::Color& amb = env->AmbientColor();
        float minAmbient = 0.08f;
        scene.ambientColor[0] = amb.red > minAmbient ? amb.red : minAmbient;
        scene.ambientColor[1] = amb.green > minAmbient ? amb.green : minAmbient;
        scene.ambientColor[2] = amb.blue > minAmbient ? amb.blue : minAmbient;
        scene.ambientColor[3] = 1.0f;

        // Fog
        if (env->FogEnable()) {
            scene.fogEnabled = 1.0f;
            scene.fogStart = env->FogStart();
            scene.fogEnd = env->FogEnd();
            const Hmx::Color& fc = env->FogColor();
            scene.fogColor[0] = fc.red;
            scene.fogColor[1] = fc.green;
            scene.fogColor[2] = fc.blue;
        }

        // Collect directional lights from environment approx list AND venue WorldDir.
        // Only take lights with non-zero color (LightPresets may leave some at black).
        // Sort by brightness and pick the top 4.
        struct LightCandidate {
            float dir[3];
            float color[3];
            float brightness;
            const char* name;
        };
        std::vector<LightCandidate> candidates;
        candidates.reserve(16);

        auto addLight = [&](RndLight* light, const float* sceneCenter) {
            const Hmx::Color& lc = light->GetColor();
            if (lc.red < 0.01f && lc.green < 0.01f && lc.blue < 0.01f) return;
            LightCandidate c;
            const Transform& lxfm = light->WorldXfm();
            if (light->GetType() == RndLight::kDirectional) {
                c.dir[0] = lxfm.m.y.x;
                c.dir[1] = lxfm.m.y.y;
                c.dir[2] = lxfm.m.y.z;
            } else {
                // Point light: approximate direction as light→sceneCenter
                float dx = sceneCenter[0] - lxfm.v.x;
                float dy = sceneCenter[1] - lxfm.v.y;
                float dz = sceneCenter[2] - lxfm.v.z;
                float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len < 0.001f) return;
                c.dir[0] = dx / len;
                c.dir[1] = dy / len;
                c.dir[2] = dz / len;
            }
            c.color[0] = lc.red;
            c.color[1] = lc.green;
            c.color[2] = lc.blue;
            c.brightness = lc.red + lc.green + lc.blue;
            c.name = light->Name();
            candidates.push_back(c);
        };
        // Scene center — approximate as camera look-at or origin
        float sceneCenter[3] = {0, 1.0f, 0}; // characters are roughly at origin, ~1m tall

        // From environment's approx list (directional + point lights)
        ObjPtrList<RndLight>& approxLights = env->LightsApprox();
        for (ObjPtrList<RndLight>::iterator it = approxLights.begin();
             it != approxLights.end(); ++it) {
            RndLight* light = *it;
            if (!light || !light->Showing()) continue;
            if (light->GetType() != RndLight::kDirectional && light->GetType() != RndLight::kPoint) continue;
            addLight(light, sceneCenter);
        }

        // NOTE: Previously supplemented from venue WorldDir via ObjDirItr<RndLight>,
        // but recursive dir iteration is too expensive in WASM (dynamic_cast on every
        // object) and can hang on corrupted hash table chains. The environment's light
        // lists + fallback defaults below are sufficient.

        // Smart light selection: prefer default/stage lights (base illumination)
        // over rim/peak/area accent lights which are designed for specific moments.
        // Check rim/peak/backup FIRST since they may contain other substrings.
        for (auto& c : candidates) {
            float priority = 1.0f;
            const char* name = c.name;
            if (name) {
                // Accent lights (check first — may contain "area" or "stage" substrings)
                if (strstr(name, "_rim") || strstr(name, "rim_")) priority = 0.1f;
                else if (strstr(name, "peak_") || strstr(name, "Peak")) priority = 0.2f;
                else if (strstr(name, "backup")) priority = 0.3f;
                // Base illumination lights
                else if (strstr(name, "default_") || strstr(name, "Default")) priority = 10.0f;
                else if (strstr(name, "stage") || strstr(name, "Stage")) priority = 8.0f;
                else if (strstr(name, "main") || strstr(name, "Main")) priority = 6.0f;
            }
            c.brightness *= priority;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const LightCandidate& a, const LightCandidate& b) {
                return a.brightness > b.brightness;
            });

        int lightIdx = 0;
        for (size_t i = 0; i < candidates.size() && lightIdx < 4; i++) {
            // Skip duplicates (same color within tolerance)
            bool dup = false;
            for (int di = 0; di < lightIdx; di++) {
                if (std::abs(scene.lightColors[di][0] - candidates[i].color[0]) < 0.01f &&
                    std::abs(scene.lightColors[di][1] - candidates[i].color[1]) < 0.01f &&
                    std::abs(scene.lightColors[di][2] - candidates[i].color[2]) < 0.01f) {
                    dup = true; break;
                }
            }
            if (dup) continue;
            scene.lightDirs[lightIdx][0] = candidates[i].dir[0];
            scene.lightDirs[lightIdx][1] = candidates[i].dir[1];
            scene.lightDirs[lightIdx][2] = candidates[i].dir[2];
            scene.lightDirs[lightIdx][3] = 0.0f;
            scene.lightColors[lightIdx][0] = candidates[i].color[0];
            scene.lightColors[lightIdx][1] = candidates[i].color[1];
            scene.lightColors[lightIdx][2] = candidates[i].color[2];
            scene.lightColors[lightIdx][3] = 1.0f;
            lightIdx++;
        }

        // Supplement / fallback lights — only when NOT in MILO_VIEWER mode.
        // The viewer's debug UI creates proper RndLight objects via
        // EnsureEnvironment() so every light is visible and editable.
#ifndef MILO_VIEWER
        // Supplement with fill lights if env has few directional lights
        if (lightIdx > 0 && lightIdx < 3) {
            scene.lightDirs[lightIdx][0] = 0.0f;
            scene.lightDirs[lightIdx][1] = 0.0f;
            scene.lightDirs[lightIdx][2] = 1.0f;
            scene.lightDirs[lightIdx][3] = 0.0f;
            scene.lightColors[lightIdx][0] = scene.lightColors[lightIdx][1] = scene.lightColors[lightIdx][2] = 0.35f;
            scene.lightColors[lightIdx][3] = 1.0f;
            lightIdx++;
        }
        // Fallback: if no lights found, use key + fill + rim lights
        if (lightIdx == 0) {
            scene.lightDirs[0][0] = -0.4f;
            scene.lightDirs[0][1] = -0.7f;
            scene.lightDirs[0][2] = 0.5f;
            scene.lightDirs[0][3] = 0.0f;
            scene.lightColors[0][0] = scene.lightColors[0][1] = scene.lightColors[0][2] = 0.9f;
            scene.lightColors[0][3] = 1.0f;
            scene.lightDirs[1][0] = 0.5f;
            scene.lightDirs[1][1] = -0.5f;
            scene.lightDirs[1][2] = 0.3f;
            scene.lightDirs[1][3] = 0.0f;
            scene.lightColors[1][0] = scene.lightColors[1][1] = scene.lightColors[1][2] = 0.4f;
            scene.lightColors[1][3] = 1.0f;
            scene.lightDirs[2][0] = 0.0f;
            scene.lightDirs[2][1] = 0.8f;
            scene.lightDirs[2][2] = 0.4f;
            scene.lightDirs[2][3] = 0.0f;
            scene.lightColors[2][0] = scene.lightColors[2][1] = scene.lightColors[2][2] = 0.3f;
            scene.lightColors[2][3] = 1.0f;
            lightIdx = 3;
        }
#endif
        // Cap total directional light energy to prevent overexposure.
        // DC3 venues have 30-69 lights all active simultaneously. On Xbox,
        // PropAnims in song.anim control which lights are on during gameplay.
        // Without that animation, our top-4 selection can over-expose.
        if (lightIdx > 0) {
            float totalEnergy = 0.0f;
            for (int li = 0; li < lightIdx; li++) {
                totalEnergy += scene.lightColors[li][0] + scene.lightColors[li][1] + scene.lightColors[li][2];
            }
            const float maxEnergy = 4.5f; // ~1.5 per RGB channel across all lights
            if (totalEnergy > maxEnergy) {
                float scale = maxEnergy / totalEnergy;
                for (int li = 0; li < lightIdx; li++) {
                    scene.lightColors[li][0] *= scale;
                    scene.lightColors[li][1] *= scale;
                    scene.lightColors[li][2] *= scale;
                }
            }
        }

        scene.numLights = (float)lightIdx;
        // Point lights (kPoint and kFakeSpot are in LightsReal)
        int pointIdx = 0;
        ObjPtrList<RndLight>& realLights = env->LightsReal();
        for (ObjPtrList<RndLight>::iterator it = realLights.begin();
             it != realLights.end() && pointIdx < 4; ++it) {
            RndLight* light = *it;
            if (!light || !light->Showing()) continue;
            if (light->GetType() != RndLight::kPoint) continue;

            const Transform& lxfm = light->WorldXfm();
            scene.pointLightPos[pointIdx][0] = lxfm.v.x;
            scene.pointLightPos[pointIdx][1] = lxfm.v.y;
            scene.pointLightPos[pointIdx][2] = lxfm.v.z;
            scene.pointLightPos[pointIdx][3] = 0.0f;

            const Hmx::Color& lc = light->GetColor();
            scene.pointLightColors[pointIdx][0] = lc.red;
            scene.pointLightColors[pointIdx][1] = lc.green;
            scene.pointLightColors[pointIdx][2] = lc.blue;
            scene.pointLightColors[pointIdx][3] = 1.0f;

            scene.pointLightRanges[pointIdx] = light->Range();
            pointIdx++;
        }
        // NOTE: Previously supplemented point lights from venue WorldDir via
        // ObjDirItr — removed for same reason as directional lights above.
        scene.numPointLights = (float)pointIdx;

        // Projected lights (kFakeSpot with gobo textures from LightsReal)
        mProjLightTexView = nullptr;
        for (ObjPtrList<RndLight>::iterator it = realLights.begin();
             it != realLights.end(); ++it) {
            RndLight* light = *it;
            if (!light || !light->Showing()) continue;
            if (light->GetType() != RndLight::kFakeSpot) continue;
            if (!light->GetTexture()) continue;

            // Use the first kFakeSpot with a texture as the projected light
            const Transform& lxfm = light->WorldXfm();
            // Direction: light points along -Y axis of its local frame
            scene.projLightDir[0] = -lxfm.m.y.x;
            scene.projLightDir[1] = -lxfm.m.y.y;
            scene.projLightDir[2] = -lxfm.m.y.z;
            scene.projLightDir[3] = 0.0f;

            const Hmx::Color& lc = light->GetColor();
            scene.projLightColor[0] = lc.red;
            scene.projLightColor[1] = lc.green;
            scene.projLightColor[2] = lc.blue;
            scene.projLightColor[3] = 1.0f;

            // Compute projection matrix and extract UV rows
            Transform proj = light->Projection();
            // Row 0 (u): column-major transform → row of transposed matrix
            scene.projLightProjRow0[0] = proj.m.x.x;
            scene.projLightProjRow0[1] = proj.m.y.x;
            scene.projLightProjRow0[2] = proj.m.z.x;
            scene.projLightProjRow0[3] = proj.v.x;
            // Row 1 (v)
            scene.projLightProjRow1[0] = proj.m.x.y;
            scene.projLightProjRow1[1] = proj.m.y.y;
            scene.projLightProjRow1[2] = proj.m.z.y;
            scene.projLightProjRow1[3] = proj.v.y;

            scene.numProjLights = 1.0f;
            mProjLightTexView = GetGpuTexView(light->GetTexture());
            break;  // only 1 projected light supported
        }

    } else {
        // Default lighting — three-point light setup
        scene.ambientColor[0] = scene.ambientColor[1] = scene.ambientColor[2] = 0.4f;
        scene.ambientColor[3] = 1.0f;
        // Key (front-facing for character visibility)
        scene.lightDirs[0][0] = 0.5f;
        scene.lightDirs[0][1] = 0.3f;
        scene.lightDirs[0][2] = -0.7f;
        scene.lightDirs[0][3] = 0.0f;
        scene.lightColors[0][0] = scene.lightColors[0][1] = scene.lightColors[0][2] = 1.1f;
        scene.lightColors[0][3] = 1.0f;
        // Fill
        scene.lightDirs[1][0] = 0.5f;
        scene.lightDirs[1][1] = -0.5f;
        scene.lightDirs[1][2] = 0.3f;
        scene.lightDirs[1][3] = 0.0f;
        scene.lightColors[1][0] = scene.lightColors[1][1] = scene.lightColors[1][2] = 0.4f;
        scene.lightColors[1][3] = 1.0f;
        // Rim
        scene.lightDirs[2][0] = 0.0f;
        scene.lightDirs[2][1] = 0.8f;
        scene.lightDirs[2][2] = 0.4f;
        scene.lightDirs[2][3] = 0.0f;
        scene.lightColors[2][0] = scene.lightColors[2][1] = scene.lightColors[2][2] = 0.3f;
        scene.lightColors[2][3] = 1.0f;
        scene.numLights = 3.0f;
    }

    // Shadow mapping data
    if (mShadowPass.Available()) {
        memcpy(scene.lightViewProj, mShadowPass.LightViewProj(), 64);
        scene.shadowEnabled = 1.0f;
        scene.shadowBias = 0.002f;
        scene.shadowMapSize = (float)ShadowPass::kShadowMapSize;
        scene.shadowStrength = 0.3f;  // minimum brightness in shadow
    }

    // Upload scene uniforms at a new ring buffer offset (allows mid-frame camera switches —
    // each camera gets its own data in the ring, so queue.WriteBuffer doesn't overwrite earlier values)
    uint32_t sceneOffset = mSceneRing.Write(mGpu.Queue(), &scene, sizeof(scene));
    mLastSceneOffset = sceneOffset;

    // Create scene bind group (group 0) — shadow map + projected light texture
    wgpu::BindGroupEntry entries[5] = {};
    entries[0].binding = 0;
    entries[0].buffer = mSceneRing.Buffer();
    entries[0].offset = sceneOffset;
    entries[0].size = sizeof(SceneUniforms);

    entries[1].binding = 1;
    entries[1].textureView = mShadowPass.DepthView();  // always valid after Init

    entries[2].binding = 2;
    entries[2].sampler = mShadowPass.Sampler();  // comparison sampler

    entries[3].binding = 3;
    entries[3].textureView = mProjLightTexView ? mProjLightTexView : mWhiteTexView;

    entries[4].binding = 4;
    entries[4].sampler = mDefaultSampler;

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.label = "SceneBindGroup";
    bgDesc.layout = mPipelines.SceneLayout();
    bgDesc.entryCount = 5;
    bgDesc.entries = entries;
    mSceneBindGroup = mGpu.Device().CreateBindGroup(&bgDesc);
}

wgpu::BindGroup WgpuRnd::CreateMaterialBindGroup(
    uint32_t bufferOffset, uint32_t bufferSize,
    const MaterialTexViews& texViews,
    wgpu::Sampler& diffuseSampler, wgpu::Sampler& mapSampler)
{
    wgpu::BindGroupEntry entries[11] = {};

    entries[0].binding = 0;
    entries[0].buffer = mMaterialRing.Buffer();
    entries[0].offset = bufferOffset;
    entries[0].size = bufferSize;

    entries[1].binding = 1;
    entries[1].textureView = texViews.diffuse;

    entries[2].binding = 2;
    entries[2].sampler = diffuseSampler;

    entries[3].binding = 3;
    entries[3].textureView = texViews.normal;

    entries[4].binding = 4;
    entries[4].textureView = texViews.specular;

    entries[5].binding = 5;
    entries[5].textureView = texViews.emissive;

    entries[6].binding = 6;
    entries[6].textureView = texViews.rim;

    entries[7].binding = 7;
    entries[7].sampler = mapSampler;

    entries[8].binding = 8;
    entries[8].textureView = texViews.environCube;

    entries[9].binding = 9;
    entries[9].sampler = mapSampler;  // reuse map sampler for cube

    entries[10].binding = 10;
    entries[10].textureView = texViews.normDetail;

    wgpu::BindGroupDescriptor desc{};
    desc.layout = mPipelines.MaterialLayout();
    desc.entryCount = 11;
    desc.entries = entries;

    return mGpu.Device().CreateBindGroup(&desc);
}

wgpu::BindGroup WgpuRnd::CreateObjectBindGroup(uint32_t bufferOffset, uint32_t bufferSize) {
    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = mObjectRing.Buffer();
    entry.offset = bufferOffset;
    entry.size = bufferSize;

    wgpu::BindGroupDescriptor desc{};
    desc.layout = mPipelines.ObjectLayout();
    desc.entryCount = 1;
    desc.entries = &entry;

    return mGpu.Device().CreateBindGroup(&desc);
}

wgpu::BindGroup WgpuRnd::CreateBoneBindGroup(uint32_t bufferOffset, uint32_t bufferSize) {
    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = mBoneRing.Buffer();
    entry.offset = bufferOffset;
    entry.size = bufferSize;

    wgpu::BindGroupDescriptor desc{};
    desc.layout = mPipelines.BoneLayout();
    desc.entryCount = 1;
    desc.entries = &entry;

    return mGpu.Device().CreateBindGroup(&desc);
}

#ifdef HX_IMGUI
void WgpuRnd::RenderImGuiOverlay() {
    // Only render if ImGui has draw data ready (NewFrame + Render already called)
    if (!ImGui::GetCurrentContext()) return;
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->TotalVtxCount == 0) return;

    // Start a simple pass directly to the frame target (no MSAA for ImGui — simpler)
    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = FrameTarget();
    colorAtt.loadOp = wgpu::LoadOp::Load; // preserve everything rendered so far
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "ImGuiOverlayPass";
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;

    wgpu::RenderPassEncoder imguiPass = mEncoder.BeginRenderPass(&rpDesc);
    ImGui_ImplWGPU_RenderDrawData(drawData, imguiPass.Get());
    imguiPass.End();
}
#endif

void WgpuRnd::MaybeCaptureFrame() {
    if (mCaptureIndex >= (int)mCaptureFrames.size()) return;
    if (mFrameID != mCaptureFrames[mCaptureIndex]) return;

    int w = mGpu.WindowWidth();
    int h = mGpu.WindowHeight();
    size_t pixelSize = (size_t)w * h * 4;
    uint8_t* pixels = (uint8_t*)malloc(pixelSize);
    if (!pixels) return;

    if (mGpu.ReadbackHeadlessFrame(pixels, pixelSize)) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05d.png", mScreenshotDir.c_str(), mFrameID);
        if (WriteScreenshot(path, pixels, w, h)) {
            printf("DC3 Native: captured frame %d -> %s\n", mFrameID, path);
        } else {
            fprintf(stderr, "DC3 Native: failed to write screenshot %s\n", path);
        }
    } else {
        fprintf(stderr, "DC3 Native: failed to readback frame %d (headless mode required)\n", mFrameID);
    }

    free(pixels);
    mCaptureIndex++;
}

void WgpuRnd::MaybeEncodeVideoFrame() {
    if (!mVideoPixels) return;
    if (mGpu.ReadbackHeadlessFrame(mVideoPixels, mVideoPixelSize)) {
        mVideoEncoder.WriteFrame(mVideoPixels, mVideoPixelSize);
        if (mVideoEncoder.FrameCount() % 150 == 0) {
            printf("DC3 Video: encoded %d frames\n", mVideoEncoder.FrameCount());
        }
    }
}

// ============================================================================
// DrawRect — screen-space 2D textured/colored quad (delegates to DrawRect2D)
// ============================================================================

void WgpuRnd::DrawRect(const Hmx::Rect& rect, RndMat* mat, ShaderType,
                        const Hmx::Color& color, const Hmx::Color* topRight,
                        const Hmx::Color* botLeft) {
    if (!mInPass) return;
    mDrawRect2D.Draw(mPass, rect, mat, color, topRight, botLeft,
                     mGpu, mPipelines, mWhiteTexView, mDefaultSampler);
    // DrawRect2D uses its own pipeline/bind group at slot 0.
    // Restore the scene bind group so subsequent mesh draws don't mismatch.
    if (mSceneBindGroup)
        mPass.SetBindGroup(0, mSceneBindGroup);
}
