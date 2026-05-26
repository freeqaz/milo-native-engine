#pragma once
#include "gfx/BloomPass.h"
#include "gfx/DofPass.h"
#include <webgpu/webgpu_cpp.h>
#include <chrono>

class GpuDevice;

class PostProcPass {
public:
    void Init(GpuDevice& gpu);
    void Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
             wgpu::Texture& intermediateTex, int intermediateW, int intermediateH,
             wgpu::TextureView& depthView, wgpu::TextureView& frameView,
             wgpu::TextureView& blackTexView, GpuDevice& gpu);
    void Terminate();

    BloomPass& Bloom() { return mBloom; }
    DofPass& Dof() { return mDof; }

private:
    void EnsurePipeline(GpuDevice& gpu);

    BloomPass mBloom;
    DofPass mDof;

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
};
