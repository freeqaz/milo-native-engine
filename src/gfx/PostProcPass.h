#pragma once
#include "gfx/BloomPass.h"
#include "gfx/DofPass.h"
#include "gfx/RB3RetailPost.h"
#include <webgpu/webgpu_cpp.h>
#include <chrono>

class GpuDevice;
class RndPostProc;

// The composite draws the image's texture noise (NgPostProc::CheckNoise:
// mNoiseMap overlay-blended, see rndshape::PostProcNoiseMap) for a consumer
// whose RndPostProc exposes the map. Consumers' tests key on this.
#define MILO_POSTPROC_NOISE_MAP 1

class PostProcPass {
public:
    void Init(GpuDevice& gpu);
    // `depthView` is a depth-only view of the scene depth (`depthSamples`
    // samples, intermediate-sized); depth of field reads it.
    void Run(wgpu::CommandEncoder& encoder, const wgpu::TextureView& intermediateView,
             const wgpu::Texture& intermediateTex, int intermediateW, int intermediateH,
             const wgpu::TextureView& depthView, uint32_t depthSamples,
             wgpu::TextureView& frameView, wgpu::TextureView& blackTexView, GpuDevice& gpu);
    void Terminate();
    // The post chain's bind groups (this pass, bloom, depth of field, the
    // retail chains) come from `cache` -- WgpuRnd's post cache, aged and swept
    // once per frame -- so a steady frame creates none (dc3 PLAN H-66). Null
    // (the default): every pass creates its groups, as before.
    void SetBindGroupCache(BindGroupCache* cache) {
        mBindGroups = cache;
        mBloom.SetBindGroupCache(cache);
        mDof.SetBindGroupCache(cache);
        mRetail.SetBindGroupCache(cache);
    }
    // RB3: copies a graded frame over the current pass (RB3RetailPost::Blit).
    void BlitRetail(wgpu::RenderPassEncoder& pass, const wgpu::TextureView& src,
                    uint32_t samples, wgpu::TextureFormat depthFmt, GpuDevice& gpu) {
        mRetail.Blit(pass, src, samples, depthFmt, gpu);
    }

    // This frame's spotlight beams for the RB3 composite (RetailPostParams::
    // spotBeams..spotSmoke; gfx/SpotBeamPass). Consumed by the next Run.
    void SetSpotBeams(const wgpu::TextureView& beams, const wgpu::TextureView& fog,
                      const float k[3]) {
        mSpotBeams = beams;
        mSpotFog = fog;
        for (int i = 0; i < 3; i++) mSpotK[i] = k[i];
    }

    // np-gfx-post round 3, DC3 (rndshape::kDC3RetailPost): this frame's
    // RndVelocityBuffer::Draw result (NgPostProc::DoVelocity), consumed by the
    // next Run. `viewProj` is the world camera's (CamSceneMatrices: row-major,
    // row vectors), recorded on every call as the next call's previous frame,
    // as Draw records mViewProjXfm in its cache slot whether or not it is ready.
    void SetVelocity(bool ready, float scale, const float viewProj[16]);
    const RB3RetailPost& Retail() const { return mRetail; }

    BloomPass& Bloom() { return mBloom; }
    DofPass& Dof() { return mDof; }

private:
    void EnsurePipeline(GpuDevice& gpu);
    float StepTime();
    float StepFlicker(RndPostProc* pp, float dt);
    // NgPostProc::CheckNoise's constants for `pp` (false: no noise this frame).
    bool NoiseConstants(RndPostProc* pp, float seeds[4], float params[4],
                        wgpu::TextureView& map, bool& midtone);
    // np-gfx-post: DC3's retail chain (rndshape::kDC3RetailPost).
    // A template so that the RB3-Wii shape, which has no such post-process,
    // never instantiates it.
    template <class P>
    void RunDC3Retail(wgpu::CommandEncoder& encoder, P* pp, int mode,
                      const wgpu::TextureView& sceneView, const wgpu::TextureView& intermediateView,
                      int w, int h, const wgpu::TextureView& depthView, uint32_t depthSamples,
                      wgpu::TextureView& frameView, GpuDevice& gpu);
    const RndPostProc* mLoggedPostProc = nullptr;   // the last post-process logged

    BloomPass mBloom;
    DofPass mDof;
    RB3RetailPost mRetail;   // RB3 (kRetailPostChain) and DC3 (kDC3RetailPost) retail chains
    wgpu::TextureView mSpotBeams, mSpotFog;  // SetSpotBeams, this frame only
    float mSpotK[3] = {};
    // SetVelocity: this frame's (pending until Run) and the previous call's VP.
    bool mVelPending = false, mVelReady = false, mHavePrevVP = false;
    float mVelScale = 0.0f;
    float mVelReproj[16] = {};
    double mPrevVP[16] = {};
    int mVelReadyRun = 0, mVelCutLogs = 0;

    BindGroupCache* mBindGroups = nullptr;
    wgpu::ShaderModule mPostProcShader;
    wgpu::BindGroupLayout mPostProcBGL;
    wgpu::PipelineLayout mPostProcPipelineLayout;
    wgpu::RenderPipeline mPostProcPipeline;
    wgpu::Buffer mPostProcUniformBuffer;
    wgpu::Sampler mDefaultSampler;
    wgpu::Sampler mNoiseSampler;
    bool mPostProcReady = false;

    // NgPostProc::mRandomSeed: the stationary noise's seed pair, rolled when
    // the selected post-process changes (as NgPostProc::Select rolls it).
    const RndPostProc* mNoiseSeedOwner = nullptr;
    float mStationarySeed[2] = {0.0f, 0.0f};

    // Flicker state
    float mFlickerTarget = 1.0f;
    float mFlickerCurrent = 1.0f;
    float mFlickerTimer = 0.0f;
    std::chrono::steady_clock::time_point mLastTime{};
    bool mTimeInit = false;
    float mNoiseTime = 0.0f;
};
