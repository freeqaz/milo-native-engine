#pragma once
#include <webgpu/webgpu_cpp.h>
#include "obj/Object.h"

class GpuDevice;

class BloomPass {
public:
    void Init(GpuDevice& gpu);
    void Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
             int sceneW, int sceneH, float intensity, float threshold,
             const Hmx::Color& tint, GpuDevice& gpu);
    void Terminate();

    wgpu::TextureView& OutputView() { return mBloomView[0]; }
    bool HasOutput() const { return mBloomView[0] != nullptr; }

private:
    void EnsurePipelines(GpuDevice& gpu);
    void EnsureTextures(int sceneW, int sceneH, GpuDevice& gpu);

    static constexpr int kBloomMips = 4;
    wgpu::Texture mBloomTex[kBloomMips];
    wgpu::TextureView mBloomView[kBloomMips];
    wgpu::Texture mBloomTempTex[kBloomMips];
    wgpu::TextureView mBloomTempView[kBloomMips];
    int mBloomWidth[kBloomMips] = {};
    int mBloomHeight[kBloomMips] = {};
    wgpu::ShaderModule mBloomShader;
    wgpu::BindGroupLayout mBloomBGL;
    wgpu::PipelineLayout mBloomPipelineLayout;
    wgpu::RenderPipeline mBloomThresholdPipeline;
    wgpu::RenderPipeline mBloomBlurHPipeline;
    wgpu::RenderPipeline mBloomBlurVPipeline;
    wgpu::RenderPipeline mBloomDownsamplePipeline;
    wgpu::RenderPipeline mBloomUpsamplePipeline;
    wgpu::Buffer mBloomUniformBuffer;
    wgpu::Sampler mDefaultSampler;
    bool mBloomReady = false;
};
