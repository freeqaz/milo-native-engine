#pragma once

#include <webgpu/webgpu_cpp.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
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
    // Enhanced cutout coverage (MaterialCutCoverage == Sharpened): the
    // fs_cutout entry, whose sample mask is the texel's sharpened coverage
    // (standard_wgsl.inc kCutoutCoverage). Only in a multisampled pass.
    bool cutoutCoverage = false;
    int32_t depthBias = 0;  // positive = push away from camera (loses z-test vs unbiased)
    // The standard shader's 2D material texture samples take the look's LOD
    // bias (kTakesLodBias; the value is the scene block's materialLodBias,
    // per pass); false: unbiased. Only a look that has a bias (DC3's
    // enhanced look, PipelineManager::MaterialTakesLodBias) sets this, for
    // world draws; text and HUD keep the original's unbiased samples.
    bool lodBias = false;
    // np-look34: the text/HUD bias this uiClass draw reads (kTakesUiBias; 0
    // none, 1 glyph page, 2 other text/HUD art). Only a look with a text rule
    // (PipelineManager::UiTakesLodBias, DC3 enhanced) sets it; the value is
    // the scene block's uiLodBias / uiArtLodBias.
    uint8_t uiBias = 0;
    // Text/HUD draw into a render-scaled frame: the standard shader samples
    // its diffuse texture at output-pixel centres (kOutputPixelScale =
    // PipelineManager::OutputPixelScale); false: at the fragment.
    bool outputPixelSampling = false;
    // The kOutputPixelScale such a pipeline bakes, part of the cache key so a
    // pipeline made at one render scale is kept, not dropped, when the window
    // is resized across the step. 1 unless outputPixelSampling; a sampling
    // key left at 1 takes OutputPixelScale() when it comes in (GetPipeline,
    // WarmAsync: CacheKey), and the warm gives the other scale explicitly.
    float outputPixelScale = 1.0f;

    bool operator==(const PipelineKey& o) const {
        return shaderType == o.shaderType && blend == o.blend && zMode == o.zMode &&
               cull == o.cull && stencil == o.stencil && layout == o.layout &&
               targetFormat == o.targetFormat && sampleCount == o.sampleCount &&
               hasDepth == o.hasDepth && alphaCut == o.alphaCut &&
               alphaWrite == o.alphaWrite && alphaToCoverage == o.alphaToCoverage &&
               cutoutCoverage == o.cutoutCoverage &&
               depthBias == o.depthBias && lodBias == o.lodBias && uiBias == o.uiBias &&
               outputPixelSampling == o.outputPixelSampling &&
               outputPixelScale == o.outputPixelScale;
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
        h ^= std::hash<bool>{}(k.cutoutCoverage) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(k.depthBias) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.lodBias) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.uiBias) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.outputPixelSampling) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(k.outputPixelScale) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// True when the standard shader's alpha fringe fade (rgb *= a / 0.1 for
// a < 0.1) must not run under this blend (BaseMaterial::Blend / WgpuBlend
// numbering, which are identical): Multiply and Darken, whose identity is not
// black and which ignore source alpha. Faded, a white alpha-0 Multiply quad
// (DC3's mod_frame_debloom.mesh, bloom_block.mat) blacks out what is under it.
// PipelineManager::CreatePipeline feeds it to the shader's kSkipFringeFade.
bool BlendSkipsFringeFade(int blend);

class PipelineManager {
public:
    PipelineManager() = default;
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;
    ~PipelineManager() {
        for (auto& entry : mWarmPending) ReleasePending(entry.second);
    }
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

    // Chunked pre-warm: spreads the ~240-pipeline PreWarm() compile burst across
    // many frames so no single frame blocks (~590 ms web / ~700 ms native in one
    // shot — a guaranteed audio under-run, the Track-C song-start GPUTask).
    //
    // CRITICAL on web: wgpu::Device::CreateRenderPipeline is ASYNC over the Dawn
    // wire — the client call only writes a wire command (~0 ms) and the real
    // GPU-process SPIR-V/pipeline compile happens when the command buffer is
    // FLUSHED (once per rAF, at end-of-frame ProcessEvents). So a wall-time budget
    // on the client cannot see the compile cost; chunking must be COUNT-based so
    // each frame's flush carries only `maxThisCall` pipelines and the GPU compiles
    // them in a small per-frame batch instead of one 240-pipeline flush.
    //
    // The first call enumerates the full key space into mPreWarmKeys (same superset
    // as PreWarm); each call then creates up to `maxThisCall` keys (also stopping if
    // the optional `budgetMs` wall budget trips — a native-side safety, near-inert
    // on web), advancing mPreWarmCursor. Returns keys REMAINING (0 == done); the
    // caller stops once it returns 0. Always makes ≥1 key of forward progress.
    int PreWarmStep(wgpu::TextureFormat mainFmt, wgpu::TextureFormat rtFmt,
                    int maxThisCall, float budgetMs = 0.f);
    bool PreWarmDone() const { return mPreWarmStarted && mPreWarmCursor >= (int)mPreWarmKeys.size(); }

    // Load-time warm (dc3 PLAN H-104; engine doc section 39). WarmAsync queues
    // `key` to be compiled before anything draws with it; PollWarms (WgpuRnd
    // calls it once a frame) issues at most kWarmIssuePerFrame of the queue
    // and takes in the ones that finished. Each is a CreateRenderPipelineAsync,
    // so the frame -- a loading screen -- keeps moving: natively compiled on
    // Dawn's worker threads, on the web by the browser's GPU process off its
    // present path (a plain create there is cheap on the page's thread but
    // compiled before the GPU process presents again: 8 a frame held a web
    // loading frame for 416 ms). A draw that asks for a key still in flight
    // waits for it in GetPipeline natively; the web cannot wait, so it creates
    // that same key there and the late async result is dropped. Either way it
    // never skips, and never draws with another pipeline; one still queued is
    // created there as before. False when the key already exists, or is
    // queued or in flight.
    static constexpr int kWarmIssuePerFrame = 8;
    bool WarmAsync(const PipelineKey& key);
    void PollWarms();
    size_t WarmsOutstanding() const { return mWarmQueue.size() + mWarmPending.size(); }
    struct WarmStats {
        uint64_t queued = 0;      // WarmAsync calls that queued a key
        uint64_t finished = 0;    // compiled and in the cache
        uint64_t failed = 0;      // async create reported an error (then made at draw)
        uint64_t waited = 0;      // draws that waited for one in flight
        double waitMs = 0.0;      // ... for this long, on the drawing thread
        uint64_t takenEarly = 0;  // draws that found theirs still queued (created there)
        uint64_t drawn = 0;       // warmed keys a draw has asked for since
    };
    const WarmStats& GetWarmStats() const { return mWarmStats; }
    // Async creates whose entry has not yet been deleted, across every
    // PipelineManager: in flight, done but not yet taken in, or abandoned and
    // waiting for their callback. For the lifetime tests.
    static int LivePendingWarms() { return LiveCount(); }

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
        DrainWarms();
        mPipelineCache.clear();
        mShaderCache.clear();
        mPipelineLayout = nullptr;
        for (auto& l : mLayouts) l = nullptr;
        mDevice = nullptr;
    }

    // State mapping (public for DrawRect; the depth-stencil one for the test
    // that holds DrawChangesNothing to the states a pipeline is built with)
    wgpu::BlendState MapBlend(WgpuBlend blend);
    wgpu::DepthStencilState MapDepthStencil(WgpuZMode z, WgpuStencil s);

    // Blend the destination alpha with MAX for every blended mode, as RB3's
    // and DC3's retail DxRnd::SetDefaultRenderStates set it (BlendOpAlpha 3).
    // Off by default: alpha replaces. Set once, before any pipeline exists;
    // the pipeline caches do not key on it. See MapBlend.
    void SetAlphaBlendMax(bool on) { mAlphaBlendMax = on; }
    bool AlphaBlendMax() const { return mAlphaBlendMax; }

    // The uniform buffer of the material (group 1, binding 0), object (group
    // 2) and bone (group 3) layouts takes a dynamic offset (PLAN H-30), so one
    // bind group serves every draw that binds the same resources and the
    // per-draw uniform block is reached by the offset given to SetBindGroup.
    // Every SetBindGroup on those groups must then pass exactly one dynamic
    // offset. Off by default (RB3's BandRnd and rb3's tools build their bind
    // groups with the offset in the entry). Set before Init.
    void SetDynamicUniformOffsets(bool on) { mDynamicUniformOffsets = on; }
    bool DynamicUniformOffsets() const { return mDynamicUniformOffsets; }

    // Whether the standard shader's alpha fringe fade runs at all
    // (rndshape::kAlphaFringeFade). Off, every pipeline is created with
    // kSkipFringeFade = 1 -- the override that already exists per blend
    // (BlendSkipsFringeFade), so this adds no pipeline variant. On by default:
    // the consumer's renderer unchanged. Set once, before any pipeline exists.
    void SetAlphaFringeFade(bool on) { mAlphaFringeFade = on; }
    bool AlphaFringeFade() const { return mAlphaFringeFade; }
    // The kSkipFringeFade a pipeline under `blend` is created with.
    bool SkipsFringeFade(int blend) const { return !mAlphaFringeFade || BlendSkipsFringeFade(blend); }
    // Whether world draws' pipelines take a LOD bias on the standard shader's
    // 2D material texture samples (standard_wgsl.inc kTakesLodBias; cube,
    // projector and shadow samples are never biased): the look has one
    // (milo::LookTakesLodBias). False by default -- every consumer's
    // original samples unbiased. Set once, before any pipeline exists. The
    // bias's VALUE is not a pipeline constant: it is the scene block's
    // materialLodBias, written per pass, so it can follow the render scale
    // without a pipeline being made.
    void SetMaterialTakesLodBias(bool takes) { mMaterialTakesLodBias = takes; }
    bool MaterialTakesLodBias() const { return mMaterialTakesLodBias; }
    // np-look34: whether text/HUD pipelines read the text bias
    // (milo::LookTakesUiLodBias; PipelineKey::uiBias). False by default.
    void SetUiTakesLodBias(bool takes) { mUiTakesLodBias = takes; }
    bool UiTakesLodBias() const { return mUiTakesLodBias; }
    // The kTakesLodBias a pipeline for `key` is created with (1 or 0).
    float LodBiasFlagFor(const PipelineKey& key) const { return key.lodBias ? 1.0f : 0.0f; }
    // A render-scaled frame's pixels per output pixel (1 = not scaled; the
    // renderer sets it from GpuDevice each frame), and the kOutputPixelScale a
    // pipeline for `key` is created with now. A change (a window resized
    // across a render-scale step) drops nothing: the scale is part of the
    // cache key (PipelineKey::outputPixelScale), so the pipelines made at the
    // other scale are still there when the window comes back to it.
    void SetOutputPixelScale(float scale) { mOutputPixelScale = scale; }
    float OutputPixelScale() const { return mOutputPixelScale; }
    float OutputPixelScaleFor(const PipelineKey& key) const {
        return key.outputPixelSampling ? mOutputPixelScale : 1.0f;
    }
    // `key` as the cache holds it: a sampling key's outputPixelScale as given
    // (> 1), else the scale now; 1 for every other key.
    PipelineKey CacheKey(const PipelineKey& key) const {
        PipelineKey k = key;
        k.outputPixelScale = !key.outputPixelSampling ? 1.0f
                           : key.outputPixelScale > 1.0f ? key.outputPixelScale : mOutputPixelScale;
        return k;
    }
    // The output-pixel scales a resize of the frame can give (GpuDevice::
    // RenderScaleRange; both 1 until set): a text or HUD key is warmed at
    // each (Mesh_Wgpu's warm), so a window resized across the render-scale
    // step finds those pipelines made, not only the ones of the size it
    // started at.
    void SetOutputPixelScaleRange(float lo, float hi) {
        mOutputPixelScaleLo = lo;
        mOutputPixelScaleHi = hi;
    }
    float OutputPixelScaleLo() const { return mOutputPixelScaleLo; }
    float OutputPixelScaleHi() const { return mOutputPixelScaleHi; }

private:
    struct DescStorage;
    void BuildDescriptor(const PipelineKey& key, DescStorage& st);
    wgpu::RenderPipeline CreatePipeline(const PipelineKey& key);
    // One async create in flight. mWarmPending owns it until its entry goes;
    // ReleasePending then deletes it if the callback has run, or marks it
    // abandoned and the callback deletes it when it runs (a web drain cannot
    // wait). Callbacks run only in this thread's ProcessEvents / WaitAny
    // (CallbackMode::AllowProcessEvents), so the two never race. Not a
    // shared_ptr: make_shared's control block calls type_info::operator==,
    // which libstdc++ declares out of line under -fms-compatibility and does
    // not export, so rb3-xenon (which builds the engine that way) cannot link.
    struct PendingWarm {
        PendingWarm() { ++LiveCount(); }
        ~PendingWarm() { --LiveCount(); }
        PendingWarm(const PendingWarm&) = delete;
        PendingWarm& operator=(const PendingWarm&) = delete;
        wgpu::Future future{};
        wgpu::RenderPipeline result;
        bool done = false;
        bool ok = false;
        bool abandoned = false;
    };
    // Same thread as the callbacks (see above), so a plain count.
    static int& LiveCount() {
        static int n = 0;
        return n;
    }
    static void ReleasePending(PendingWarm* pw);
    void FinishWarm(const PipelineKey& key, PendingWarm& pw);
    void DrainWarms();  // natively waits out every async create first
    std::vector<PipelineKey> mWarmQueue;
    std::unordered_set<PipelineKey, PipelineKeyHash> mWarmQueued;  // mWarmQueue's members
    std::unordered_map<PipelineKey, PendingWarm*, PipelineKeyHash> mWarmPending;
    std::unordered_set<PipelineKey, PipelineKeyHash> mWarmedUndrawn;  // made by a warm, not yet asked for
    WarmStats mWarmStats;
    wgpu::ShaderModule GetOrCreateShader(uint32_t shaderType);
    wgpu::CullMode MapCull(WgpuCull cull);

    GpuDevice* mDevice = nullptr;
    wgpu::BindGroupLayout mLayouts[4];
    wgpu::PipelineLayout mPipelineLayout;
    std::unordered_map<uint32_t, wgpu::ShaderModule> mShaderCache;
    std::unordered_map<PipelineKey, wgpu::RenderPipeline, PipelineKeyHash> mPipelineCache;

    // Chunked pre-warm state (PreWarmStep). The full enumerated key space is built
    // once on the first step; the cursor walks it across frames.
    std::vector<PipelineKey> mPreWarmKeys;
    int mPreWarmCursor = 0;
    bool mPreWarmStarted = false;
    bool mAlphaBlendMax = false;   // SetAlphaBlendMax
    bool mDynamicUniformOffsets = false;  // SetDynamicUniformOffsets
    bool mMaterialTakesLodBias = false;   // SetMaterialTakesLodBias
    bool mUiTakesLodBias = false;         // SetUiTakesLodBias
    bool mAlphaFringeFade = true;         // SetAlphaFringeFade
    float mOutputPixelScale = 1.0f;       // SetOutputPixelScale
    float mOutputPixelScaleLo = 1.0f;     // SetOutputPixelScaleRange
    float mOutputPixelScaleHi = 1.0f;
    void BuildPreWarmKeys(wgpu::TextureFormat mainFmt, wgpu::TextureFormat rtFmt);
};
