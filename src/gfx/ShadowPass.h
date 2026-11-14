#pragma once
#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

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
    // One projected light's casters: drawn with clipFromWorld (column-major)
    // into atlas tile `tile` (0..3: the depth texture as 2x2 quarters,
    // row-major from the top left).
    struct CasterSet {
        const float* clipFromWorld;
        RndMesh* const* meshes;
        int numMeshes;
        int tile;
    };
    // Draw each set's meshes into its tile at depth 0, over a cleared depth of
    // 1: a coverage map, not a depth map. The DC3 shape's projected shadow
    // lights (rndshape::CollectProjShadowCasterSets); leaves Available() and
    // LightViewProj() alone. Self-contained: its own dynamic-offset layouts,
    // pipelines and buffers (one upload per frame for all casters, bind groups
    // created once), not Mesh_Wgpu's DrawMeshShadow.
    void RenderCasters(wgpu::CommandEncoder& encoder, GpuDevice& gpu,
                       const CasterSet* sets, int numSets);
    void Terminate();

    // What RenderCasters did: bind groups created over the pass's life (the
    // scene group plus one per buffer (re)allocation -- constant once the
    // buffers fit), frames drawn, and the last frame's draws and upload bytes.
    struct CasterStats {
        uint32_t bindGroupsCreated = 0;
        uint32_t frames = 0;
        uint32_t lastDraws = 0;
        uint32_t lastTiles = 0;
        uint64_t lastUploadBytes = 0;
    };
    const CasterStats& GetCasterStats() const { return mCasterStats; }
    // The caster map itself (Depth32Float, kShadowMapSize square; CopySrc).
    wgpu::Texture& DepthTexture() { return mShadowDepthTex; }

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
    void EnsureCasterPipelines(GpuDevice& gpu);
    void EnsureCasterBuffer(GpuDevice& gpu, wgpu::Buffer& buf, uint64_t& capacity,
                            wgpu::BindGroup& group, const wgpu::BindGroupLayout& layout,
                            uint64_t bindingSize, uint64_t need, const char* label);

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

    // RenderCasters' own resources.
    wgpu::BindGroupLayout mCasterSceneBGL;
    wgpu::Buffer mCasterClipBuf;
    wgpu::BindGroupLayout mCasterObjectBGL;
    wgpu::BindGroupLayout mCasterBoneBGL;
    wgpu::RenderPipeline mCasterStaticPipeline;
    wgpu::RenderPipeline mCasterSkinnedPipeline;
    wgpu::BindGroup mCasterSceneBindGroup;
    wgpu::Buffer mCasterObjectBuf;
    wgpu::Buffer mCasterBoneBuf;
    uint64_t mCasterObjectCap = 0;
    uint64_t mCasterBoneCap = 0;
    wgpu::BindGroup mCasterObjectBindGroup;
    wgpu::BindGroup mCasterBoneBindGroup;
    std::vector<uint8_t> mCasterObjectStaging;
    std::vector<uint8_t> mCasterBoneStaging;
    bool mCasterReady = false;
    CasterStats mCasterStats;
};
