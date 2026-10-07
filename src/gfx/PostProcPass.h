#pragma once
#include "gfx/BloomPass.h"
#include "gfx/DofPass.h"
#include "gfx/RB3RetailPost.h"
#include <webgpu/webgpu_cpp.h>
#include <chrono>

class GpuDevice;
class RndPostProc;

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

    BloomPass& Bloom() { return mBloom; }
    DofPass& Dof() { return mDof; }

private:
    void EnsurePipeline(GpuDevice& gpu);
    float StepTime();
    float StepFlicker(RndPostProc* pp, float dt);

    BloomPass mBloom;
    DofPass mDof;
    RB3RetailPost mRetail;   // RB3 content only (rndshape::kRetailPostChain)
    wgpu::TextureView mSpotBeams, mSpotFog;  // SetSpotBeams, this frame only
    float mSpotK[3] = {};

    wgpu::ShaderModule mPostProcShader;
    wgpu::BindGroupLayout mPostProcBGL;
    wgpu::PipelineLayout mPostProcPipelineLayout;
    wgpu::RenderPipeline mPostProcPipeline;
    wgpu::Buffer mPostProcUniformBuffer;
    wgpu::Sampler mDefaultSampler;
    bool mPostProcReady = false;

    // Flicker state
    float mFlickerTarget = 1.0f;
    float mFlickerCurrent = 1.0f;
    float mFlickerTimer = 0.0f;
    std::chrono::steady_clock::time_point mLastTime{};
    bool mTimeInit = false;
    float mNoiseTime = 0.0f;
};
