#pragma once
#include <webgpu/webgpu_cpp.h>

class GpuDevice;
class PipelineManager;
class UniformRingBuffer;
class RndEnviron;
class RndCam;
class RndMesh;

class ShadowPass {
public:
    void Init(GpuDevice& gpu);
    void Render(wgpu::CommandEncoder& encoder, UniformRingBuffer& objectRing,
                UniformRingBuffer& boneRing, GpuDevice& gpu);
    void Terminate();

    // Outputs consumed by main pass
    wgpu::TextureView& DepthView() { return mShadowDepthView; }
    wgpu::Sampler& Sampler() { return mShadowSampler; }
    bool Available() const { return mShadowAvailable; }
    const float* LightViewProj() const { return mLightViewProj; }

    // For Mesh_Wgpu shadow draw
    bool InShadowPass() const { return mInShadowPass; }
    wgpu::RenderPassEncoder& Pass() { return mShadowPass; }
    wgpu::RenderPipeline& StaticPipeline() { return mShadowStaticPipeline; }
    wgpu::RenderPipeline& SkinnedPipeline() { return mShadowSkinnedPipeline; }
    wgpu::BindGroupLayout& ObjectBGL() { return mShadowObjectBGL; }
    wgpu::BindGroupLayout& BoneBGL() { return mShadowBoneBGL; }

    static constexpr int kShadowMapSize = 1024;

private:
    void EnsurePipelines(GpuDevice& gpu);

    wgpu::Texture mShadowDepthTex;
    wgpu::TextureView mShadowDepthView;
    wgpu::Sampler mShadowSampler;
    wgpu::ShaderModule mShadowShader;
    wgpu::BindGroupLayout mShadowSceneBGL;
    wgpu::BindGroupLayout mShadowObjectBGL;
    wgpu::BindGroupLayout mShadowBoneBGL;
    wgpu::PipelineLayout mShadowPipelineLayout;
    wgpu::PipelineLayout mShadowSkinnedPipelineLayout;
    wgpu::RenderPipeline mShadowStaticPipeline;
    wgpu::RenderPipeline mShadowSkinnedPipeline;
    wgpu::Buffer mShadowLightVPBuffer;
    wgpu::BindGroup mShadowSceneBindGroup;
    bool mShadowReady = false;
    float mLightViewProj[16] = {};
    bool mShadowAvailable = false;
    bool mInShadowPass = false;
    wgpu::RenderPassEncoder mShadowPass;
};
