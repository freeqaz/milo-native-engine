// RB3 GPU backend — BandRnd. Selected when MILO_ENGINE_GPU_BACKEND=rb3.
//
// BandRnd : Rnd (RB3's rndobj/Rnd.h — RB3 has no NgRnd). Sibling of Rnd_Wgpu.h's
// WgpuRnd : NgRnd (the DC3 backend, selected when MILO_ENGINE_GPU_BACKEND=dc3).
// Both backends:
//   - own a GpuDevice + PipelineManager (engine gfx CORE — game-agnostic)
//   - read SceneUniforms/MaterialUniforms/ObjectUniforms/BoneUniforms from
//     gfx/UniformStructs.h (the shared WGSL contract — same shaders see both)
//   - emit GpuVertex (gfx/VertexFormats.h) — the unpacked vertex layout
//
// What's specific to this backend is *translation*: how RB3's 2010-era rndobj
// shapes (RndCam without GetViewProjectXfms, RndMesh::Vert with packed Color32,
// the slimmer RndMat) are read and packed into the shared structs. RB3 also
// supplies its own strong defs for the engine's weak-stubbed RndMesh::DrawShowing
// and RndTex::SyncBitmap/PresyncBitmap (CPU DXT1/3/5 decompression inline).
//
// Future-modularity note. Once a feature needs adding to BOTH backends, the
// right next refactor is to split per-rndobj "translation" out (e.g.
// Mat_Translate_DC3.cpp / Mat_Translate_RB3.cpp) so the orchestrator file stays
// small and feature parity is one TU per rndobj class per flavor. Premature
// today; flagged for when duplication first hurts.

#pragma once

#include "gfx/GpuDevice.h"
#include "gfx/PipelineManager.h"
#include "gfx/Screenshot.h"
#include "gfx/UniformStructs.h"
#include "gfx/VertexFormats.h"   // GpuVertex — RB3 uses the engine's static vert layout
#include "rndobj/Rnd.h"

#include <webgpu/webgpu_cpp.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// RB3's vertex-name alias for in-file readability. Same 64-byte layout as
// GpuVertex (decomp-agnostic, defined in gfx/VertexFormats.h).
using GpuVertexRB3 = GpuVertex;

class RndCam;
class RndMesh;
class RndMat;

// Simple bump-allocated uniform ring (mirrors Rnd_Wgpu.h UniformRingBuffer).
class BandUniformRing {
public:
    void Init(wgpu::Device device, uint32_t capacity, const char* label);
    void Reset() { mOffset = 0; }
    void Release() { mBuffer = nullptr; mDevice = nullptr; }
    uint32_t Write(wgpu::Queue queue, const void* data, uint32_t size);
    wgpu::Buffer& Buffer() { return mBuffer; }
private:
    static constexpr uint32_t kAlign = 256;
    wgpu::Device mDevice;
    wgpu::Buffer mBuffer;
    uint32_t mCapacity = 0;
    uint32_t mOffset = 0;
    const char* mLabel = "BandRing";
};

class BandRnd : public Rnd {
public:
    BandRnd() {}
    virtual ~BandRnd() {}

    // Curated PreInit: register the rndobj factories (mirrors Rnd::PreInit) and
    // create defaults, WITHOUT the GPU/TheRnd/overlay/console parts. Call before
    // loading a milo. Idempotent.
    void PreInitRender();

    // Stand up the engine GpuDevice + pipelines + uniform buffers + default
    // textures in a single blocking call. Used by NATIVE callers (Dawn makes
    // the device ready synchronously). On web the device request is async, so
    // the web boot machine drives the two-phase split below instead.
    bool InitGpu(int width, int height, bool headless);

    // Phase 1: dispatch the GpuDevice adapter/device request. Returns true once
    // dispatched (true immediately on web — device not yet ready). No GPU
    // resources are created here. Poll Gpu().IsReady() before phase 2.
    bool StartGpuInit(int width, int height, bool headless);

    // Phase 2: create pipelines + depth tex + uniform rings + default textures
    // and latch mGpuReady. Call ONLY after Gpu().IsReady(). Idempotent.
    void InitGpuResources();

    // Tear down all wgpu handles owned by this object. Must run BEFORE libc
    // static destructors (the Vulkan ICD .so is unmapped on exit() and any
    // ~wgpu::ObjectBase that drops the last ref then calls vkDestroy* via a
    // dangling function pointer). Registered as a TheDebug exit callback by
    // RB3RegisterBandRndShutdown() so it fires from Debug::Exit ahead of
    // libc exit().
    void Shutdown();

    // Per-frame: acquire target, begin pass with clear, reset rings, write scene
    // uniforms from the given camera.
    void BeginFrame(RndCam* cam);
    // End pass + submit. Does NOT present (headless readback comes from GpuDevice).
    void EndFrame();

    // Draw one RndMesh (called from the engine RndMesh::DrawShowing body).
    void DrawMesh(RndMesh* mesh);

    // --- Render-to-texture (RTT) ---
    // The shared rndobj/Cam.cpp only ever fires RndTex::FinishDrawTarget (the
    // END hook); the BEGIN side lived in the per-platform Wii/Xenon RndCam,
    // which this backend lacks. So DrawMesh hooks the begin lazily (when the
    // current cam has a TargetTex that we haven't redirected to yet) via
    // BeginDrawTarget, and RndTex::FinishDrawTarget calls EndDrawTarget.
    //
    // BeginDrawTarget: lazily create (once) an RGBA8 render target for `tex`
    // (its Width()xHeight()), suspend the main pass, and begin a NEW render
    // pass into the RT view (clear transparent, no depth). Subsequent draws
    // land in the RT until EndDrawTarget re-opens the main pass.
    void BeginDrawTarget(RndTex* tex);
    void EndDrawTarget();

    GpuDevice& Gpu() { return mGpu; }
    wgpu::RenderPassEncoder& Pass() { return mPass; }
    bool InPass() const { return mInPass; }

    // Initialise auto-screenshot from env vars (call after InitGpu).
    // Reads MILO_SCREENSHOT_DIR and MILO_SCREENSHOT_FRAMES (comma-separated
    // frame numbers); if set, captures a PNG at each listed frame number.
    // Frame names (optional) come from MILO_SCREENSHOT_NAMES (comma-separated,
    // same count as MILO_SCREENSHOT_FRAMES).
    void InitScreenshots();

    // --- Rnd virtual overrides ---
    // BeginDrawing: acquire GPU target + start render pass (using current cam).
    void BeginDrawing() override;
    // EndDrawing: end pass + submit + optionally capture screenshot.
    void EndDrawing() override;

    // DrawRect: draw one textured/color-modulated 2D quad into the CURRENTLY
    // ACTIVE pass. Used by OutfitConfig::MatSwap::Compose to paint its base +
    // two-color diffuse/interp/mask tint layers into an RTT outfit diffuse tex.
    // Self-contained (the dc3 DrawRect2D.cpp TU is excluded for the rb3 backend);
    // builds on the SHARED quad pipeline infra below (EnsureQuadPipeline +
    // mQuad*), which Stage 2's postproc composite reuses. Modulation =
    // mat->GetColor() * paramColor (Compose passes white param + sets the real
    // tint via sMat->SetColor — unlike dc3 DrawRect2D which ignores matColor).
    void DrawRect(const Hmx::Rect&, const Hmx::Color&, RndMat*,
                  const Hmx::Color*, const Hmx::Color*) override;

private:
    void WriteSceneUniforms(RndCam* cam);
    void CreateDefaultTextures();
    wgpu::BindGroup MakeMaterialBindGroup(uint32_t off, RndMat* mat);
    wgpu::BindGroup MakeMaterialBindGroupRaw(wgpu::Buffer buf, uint32_t off);

    // --- Shared 2D quad pipeline infra (§3 of the RTT engine plan) ---
    // Compile-once shader module with vs_rect/fs_rect/fs_rect_notex entries
    // (Stage 2 adds vs_fullscreen/fs_postproc to the SAME module). Build the
    // rect bind-group layout (tex@0, samp@1, RectUB@2), the rect pipeline layout,
    // the 6-vertex quad vbuf, and the 32-byte RectUB. Idempotent.
    void EnsureQuadPipeline();

public:
    GpuDevice mGpu;
    PipelineManager mPipelines;

    BandUniformRing mSceneRing;
    BandUniformRing mMaterialRing;
    BandUniformRing mObjectRing;
    BandUniformRing mBoneRing;

    wgpu::TextureFormat mTargetFmt = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::CommandEncoder mEncoder;
    wgpu::RenderPassEncoder mPass;
    wgpu::TextureView mFrameView;
    bool mInPass = false;

    wgpu::Texture mDepthTex;
    wgpu::TextureView mDepthView;

    // RTT: the texture currently being painted into (the redirected target),
    // or null when drawing into the main pass. While set, DrawMesh selects an
    // RT-compatible pipeline variant (RGBA8 color, no depth, alpha writes on).
    RndTex* mRtActiveTex = nullptr;
    wgpu::TextureFormat mRtFmt = wgpu::TextureFormat::RGBA8Unorm;

    // Scene bind group (group 0). mLastSceneCam tracks which cam the bind
    // group was last written against — DrawMesh re-writes the scene uniforms
    // when RndCam::sCurrent changes mid-frame (so panels that Select() their
    // own scene cam after BeginDrawing affect subsequent draws).
    wgpu::BindGroup mSceneBindGroup;
    uint32_t mSceneOffset = 0;
    RndCam* mLastSceneCam = nullptr;
    // Last-written camera world transform (translation + forward basis). The
    // gem highway scroll (TrackDir::DrawShowing) re-poses the SAME game.cam
    // object via SetWorldXfm between draw layers, so a pointer-only staleness
    // check misses it — the scrolled gems would then project against the
    // stationary pose and fall beyond the far plane. Track the pose so we
    // re-write scene uniforms when game.cam's transform changes mid-frame.
    float mLastSceneCamPose[6] = {0,0,0,0,0,0};

    // Default textures
    wgpu::Texture mWhiteTex, mBlackTex, mFlatNormalTex, mBlackCubeTex, mShadowTex;
    wgpu::TextureView mWhiteView, mBlackView, mFlatNormalView, mBlackCubeView, mShadowView;
    wgpu::Sampler mSampler, mShadowSampler;

    // --- Shared 2D quad pipeline infra (built by EnsureQuadPipeline) ---
    // Stage 1 (drawrect) uses mQuadRect*; Stage 2 (postproc) will add mQuadPost*
    // + mPostProcUB to this same group (left as TODO members below for reuse).
    wgpu::ShaderModule mQuadShader;
    wgpu::BindGroupLayout mQuadRectBGL;     // tex@0, samp@1, RectUB@2
    wgpu::PipelineLayout mQuadRectPL;
    wgpu::Buffer mQuadVertexBuffer;         // 6 verts x 32B (pos2/uv2/color4)
    wgpu::Buffer mRectUB;                   // 32B: mod(4) + flags(4) [+pad]
    bool mQuadReady = false;
    // Per-(format,blend,depth,isPost) RenderPipeline cache so the composite +
    // repeated DrawRect calls don't recreate a pipeline every invocation.
    std::unordered_map<uint64_t, wgpu::RenderPipeline> mQuadPipelines;

    bool mGpuReady = false;
    bool mPreInited = false;
    int mDrawnMeshes = 0;
    int mDrawnTris = 0;

    // Frame counter (incremented each EndDrawing call — the base Rnd::EndDrawing
    // is bypassed so mFrameID never advances; we keep our own).
    int mFrameCount = 0;

    // Auto-screenshot state (MILO_SCREENSHOT_DIR / MILO_SCREENSHOT_FRAMES).
    std::string mShotDir;
    std::vector<int> mShotFrames;
    std::vector<std::string> mShotNames;
    int mShotIndex = 0;
    // V4 deferral: -1 = no scene cam ever selected yet. Once a non-default
    // cam appears, latch the frame count and defer subsequent shots scheduled
    // before that frame until at least mShotSceneCamGuard frames after the
    // first scene-cam frame (so the intro's named cam is positioned).
    int mFirstSceneCamFrame = -1;
};

extern BandRnd gBandRnd;
