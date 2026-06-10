#pragma once

#include <webgpu/webgpu_cpp.h>
#include <unordered_map>
#include <cstdint>

class GpuDevice;

// Blend/ZMode/Cull enums match decomp's BaseMaterial.h values
enum class WgpuBlend {
    Dest = 0, Src = 1, Add = 2, SrcAlpha = 3, SrcAlphaAdd = 4,
    Subtract = 5, Multiply = 6, PreMultAlpha = 7, Screen = 8,
    Lighten = 9, Darken = 10,
};

enum class WgpuZMode {
    Disable = 0, Normal = 1, Transparent = 2, Force = 3, Decal = 4,
};

enum class WgpuCull {
    None = 0, Regular = 1, Backwards = 2,
};

enum class WgpuStencil {
    Ignore = 0, Write = 1, Test = 2,
};

enum class VertexLayoutType {
    Static = 0,
    Skinned = 1,
};

struct PipelineKey {
    uint32_t shaderType;        // ShaderType enum
    WgpuBlend blend;
    WgpuZMode zMode;
    WgpuCull cull;
    WgpuStencil stencil;
    VertexLayoutType layout;
    wgpu::TextureFormat targetFormat;
    uint32_t sampleCount = 1;
    bool hasDepth = true;
    bool alphaCut;
    bool alphaWrite;
    bool alphaToCoverage = false;
    int32_t depthBias = 0;  // positive = push away from camera (loses z-test vs unbiased)

    bool operator==(const PipelineKey& o) const {
        return shaderType == o.shaderType && blend == o.blend && zMode == o.zMode &&
               cull == o.cull && stencil == o.stencil && layout == o.layout &&
               targetFormat == o.targetFormat && sampleCount == o.sampleCount &&
               hasDepth == o.hasDepth && alphaCut == o.alphaCut &&
               alphaWrite == o.alphaWrite && alphaToCoverage == o.alphaToCoverage &&
               depthBias == o.depthBias;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        size_t h = 0;
        h ^= std::hash<uint32_t>{}(k.shaderType) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)k.blend) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)k.zMode) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)k.cull) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)k.layout) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}((int)k.targetFormat) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.sampleCount) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.hasDepth) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.alphaCut) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.alphaToCoverage) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(k.depthBias) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class PipelineManager {
public:
    void Init(GpuDevice* device);

    // Get or create pipeline for the given state combination
    wgpu::RenderPipeline GetPipeline(const PipelineKey& key);

    // Pre-warm: synchronously create (and cache) the small enumerable set of
    // pipeline variants the RB3 draw path (Rnd_Wgpu_RB3.cpp DrawMesh) can ask
    // for, so the first real splash-venue draw frame finds them all cache-hit
    // instead of compiling ~13 pipelines on the transition frame (87 ms native /
    // ~120 ms web async, the A5 spike). The two formats are the live main-pass
    // (mainFmt + depth) and RT-pass (rtFmt, no depth) targets — passed by the
    // backend so the warmed keys are byte-identical to the ones GetPipeline()
    // later looks up. Returns the number of pipelines actually created (misses).
    int PreWarm(wgpu::TextureFormat mainFmt, wgpu::TextureFormat rtFmt);

    // Bind group layouts (shared across all pipelines)
    wgpu::BindGroupLayout& SceneLayout() { return mLayouts[0]; }    // Group 0
    wgpu::BindGroupLayout& MaterialLayout() { return mLayouts[1]; } // Group 1
    wgpu::BindGroupLayout& ObjectLayout() { return mLayouts[2]; }   // Group 2
    wgpu::BindGroupLayout& BoneLayout() { return mLayouts[3]; }     // Group 3

    wgpu::PipelineLayout& GetPipelineLayout() { return mPipelineLayout; }

    int CachedPipelineCount() const { return (int)mPipelineCache.size(); }

    // Reload shader source from disk and recreate all pipelines.
    // Returns true on success, false if file read or compilation failed.
    bool ReloadShaders();

    // Release all GPU objects (call before device shutdown)
    void Terminate() {
        mPipelineCache.clear();
        mShaderCache.clear();
        mPipelineLayout = nullptr;
        for (auto& l : mLayouts) l = nullptr;
        mDevice = nullptr;
    }

    // State mapping (public for DrawRect)
    wgpu::BlendState MapBlend(WgpuBlend blend);

private:
    wgpu::RenderPipeline CreatePipeline(const PipelineKey& key);
    wgpu::ShaderModule GetOrCreateShader(uint32_t shaderType);
    wgpu::DepthStencilState MapDepthStencil(WgpuZMode z, WgpuStencil s);
    wgpu::CullMode MapCull(WgpuCull cull);

    GpuDevice* mDevice = nullptr;
    wgpu::BindGroupLayout mLayouts[4];
    wgpu::PipelineLayout mPipelineLayout;
    std::unordered_map<uint32_t, wgpu::ShaderModule> mShaderCache;
    std::unordered_map<PipelineKey, wgpu::RenderPipeline, PipelineKeyHash> mPipelineCache;
};
