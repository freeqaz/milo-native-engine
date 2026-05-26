#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;

class DofPass {
public:
    void Run(wgpu::CommandEncoder& encoder, wgpu::TextureView& intermediateView,
             wgpu::Texture& intermediateTex, wgpu::TextureView& depthView,
             int width, int height, GpuDevice& gpu);
    void Terminate();

private:
    void EnsurePipeline(GpuDevice& gpu);

    wgpu::Texture mDofIntermediateTex;
    wgpu::TextureView mDofIntermediateView;
    wgpu::Texture mDepthResolveTex;
    wgpu::TextureView mDepthResolveView;
    int mDofWidth = 0;
    int mDofHeight = 0;
    wgpu::ShaderModule mDofShader;
    wgpu::BindGroupLayout mDofBGL;
    wgpu::PipelineLayout mDofPipelineLayout;
    wgpu::RenderPipeline mDofPipeline;
    wgpu::RenderPipeline mDepthResolvePipeline;
    wgpu::BindGroupLayout mDepthResolveBGL;
    wgpu::PipelineLayout mDepthResolvePipelineLayout;
    wgpu::Buffer mDofUniformBuffer;
    wgpu::Sampler mDefaultSampler;
    bool mDofReady = false;
};
