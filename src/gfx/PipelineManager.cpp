#include "gfx/PipelineManager.h"
#include "gfx/GpuDevice.h"
#include "gfx/VertexFormats.h"
#include "platform/FrameTraceCounters.h"  // pulls <chrono> + the g*MsThisFrame decls

#include <cstdlib>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Frame-trace counters (FrameTraceCounters.h). WEAK fallback storage so the
// engine links standalone (milo-engine-tests); the strong rb3 defs in
// src/system/utl/Loader.cpp override these at the final rb3-native/rb3-web link.
// Clang (native) and Emscripten/Clang both honor __attribute__((weak)).
// ---------------------------------------------------------------------------
#define FT_WEAK __attribute__((weak))
FT_WEAK bool   gFrameTraceActive = false;
FT_WEAK float  gFetchSyncMsThisFrame = 0.0f;
FT_WEAK int    gFetchSyncCountThisFrame = 0;
FT_WEAK double gFetchSyncBytesThisFrame = 0.0;
FT_WEAK float  gDtaParseMsThisFrame = 0.0f;
FT_WEAK float  gObjLoadMsThisFrame = 0.0f;
FT_WEAK float  gObjLoadWorstMs = 0.0f;
FT_WEAK char   gObjLoadWorstName[64] = {0};
FT_WEAK float  gAudioPrimeMsThisFrame = 0.0f;
FT_WEAK float  gTexUploadMsThisFrame = 0.0f;
FT_WEAK int    gTexUploadCountThisFrame = 0;
FT_WEAK float  gMeshUploadMsThisFrame = 0.0f;
FT_WEAK int    gMeshUploadCountThisFrame = 0;
FT_WEAK float  gVertUnpackMsThisFrame = 0.0f;
FT_WEAK int    gVertUnpackCountThisFrame = 0;
FT_WEAK float  gPipelineCreateMsThisFrame = 0.0f;
FT_WEAK int    gPipelineCreateCountThisFrame = 0;
FT_WEAK float  gStreamReadMsThisFrame = 0.0f;
#undef FT_WEAK

// Embedded shader source — standard.wgsl is compiled into the binary
static const char* kBuiltinShaderSource =
#include "gfx/standard_wgsl.inc"
;

// Runtime-overridable shader source (set by ReloadShaders)
static std::string sLiveShaderSource;
static const char* kStandardShaderSource = kBuiltinShaderSource;

void PipelineManager::Init(GpuDevice* device) {
    mDevice = device;
    auto& dev = device->Device();

    // === Create bind group layouts ===

    // Group 0: Scene uniforms + shadow map + projected light texture
    wgpu::BindGroupLayoutEntry sceneEntries[5] = {};
    sceneEntries[0].binding = 0;
    sceneEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    sceneEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    sceneEntries[0].buffer.minBindingSize = 0;

    sceneEntries[1].binding = 1;
    sceneEntries[1].visibility = wgpu::ShaderStage::Fragment;
    sceneEntries[1].texture.sampleType = wgpu::TextureSampleType::Depth;
    sceneEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    sceneEntries[2].binding = 2;
    sceneEntries[2].visibility = wgpu::ShaderStage::Fragment;
    sceneEntries[2].sampler.type = wgpu::SamplerBindingType::Comparison;

    sceneEntries[3].binding = 3;
    sceneEntries[3].visibility = wgpu::ShaderStage::Fragment;
    sceneEntries[3].texture.sampleType = wgpu::TextureSampleType::Float;
    sceneEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    sceneEntries[4].binding = 4;
    sceneEntries[4].visibility = wgpu::ShaderStage::Fragment;
    sceneEntries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor sceneLayoutDesc{};
    sceneLayoutDesc.label = "SceneBGL";
    sceneLayoutDesc.entryCount = 5;
    sceneLayoutDesc.entries = sceneEntries;
    mLayouts[0] = dev.CreateBindGroupLayout(&sceneLayoutDesc);

    // Group 1: Material uniforms + textures + samplers
    wgpu::BindGroupLayoutEntry matEntries[11] = {};
    matEntries[0].binding = 0;
    matEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    matEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    matEntries[0].buffer.minBindingSize = 0;

    matEntries[1].binding = 1;
    matEntries[1].visibility = wgpu::ShaderStage::Fragment;
    matEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    matEntries[2].binding = 2;
    matEntries[2].visibility = wgpu::ShaderStage::Fragment;
    matEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

    // Binding 3: normal map
    matEntries[3].binding = 3;
    matEntries[3].visibility = wgpu::ShaderStage::Fragment;
    matEntries[3].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    // Binding 4: specular map
    matEntries[4].binding = 4;
    matEntries[4].visibility = wgpu::ShaderStage::Fragment;
    matEntries[4].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[4].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    // Binding 5: emissive map
    matEntries[5].binding = 5;
    matEntries[5].visibility = wgpu::ShaderStage::Fragment;
    matEntries[5].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[5].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    // Binding 6: rim map
    matEntries[6].binding = 6;
    matEntries[6].visibility = wgpu::ShaderStage::Fragment;
    matEntries[6].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[6].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    // Binding 7: shared sampler for maps 3-6
    matEntries[7].binding = 7;
    matEntries[7].visibility = wgpu::ShaderStage::Fragment;
    matEntries[7].sampler.type = wgpu::SamplerBindingType::Filtering;

    // Binding 8: environment cube map
    matEntries[8].binding = 8;
    matEntries[8].visibility = wgpu::ShaderStage::Fragment;
    matEntries[8].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[8].texture.viewDimension = wgpu::TextureViewDimension::Cube;

    // Binding 9: cube map sampler
    matEntries[9].binding = 9;
    matEntries[9].visibility = wgpu::ShaderStage::Fragment;
    matEntries[9].sampler.type = wgpu::SamplerBindingType::Filtering;

    // Binding 10: detail normal map
    matEntries[10].binding = 10;
    matEntries[10].visibility = wgpu::ShaderStage::Fragment;
    matEntries[10].texture.sampleType = wgpu::TextureSampleType::Float;
    matEntries[10].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    wgpu::BindGroupLayoutDescriptor matLayoutDesc{};
    matLayoutDesc.label = "MaterialBGL";
    matLayoutDesc.entryCount = 11;
    matLayoutDesc.entries = matEntries;
    mLayouts[1] = dev.CreateBindGroupLayout(&matLayoutDesc);

    // Group 2: Object uniforms (world transform)
    wgpu::BindGroupLayoutEntry objEntries[1] = {};
    objEntries[0].binding = 0;
    objEntries[0].visibility = wgpu::ShaderStage::Vertex;
    objEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    objEntries[0].buffer.minBindingSize = 0;

    wgpu::BindGroupLayoutDescriptor objLayoutDesc{};
    objLayoutDesc.label = "ObjectBGL";
    objLayoutDesc.entryCount = 1;
    objLayoutDesc.entries = objEntries;
    mLayouts[2] = dev.CreateBindGroupLayout(&objLayoutDesc);

    // Group 3: Bone uniforms (skinned mesh — per-draw)
    wgpu::BindGroupLayoutEntry boneEntries[1] = {};
    boneEntries[0].binding = 0;
    boneEntries[0].visibility = wgpu::ShaderStage::Vertex;
    boneEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    boneEntries[0].buffer.minBindingSize = 0;

    wgpu::BindGroupLayoutDescriptor boneLayoutDesc{};
    boneLayoutDesc.label = "BoneBGL";
    boneLayoutDesc.entryCount = 1;
    boneLayoutDesc.entries = boneEntries;
    mLayouts[3] = dev.CreateBindGroupLayout(&boneLayoutDesc);

    // === Create pipeline layout ===
    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = "MainPipelineLayout";
    plDesc.bindGroupLayoutCount = 4;
    plDesc.bindGroupLayouts = mLayouts;
    mPipelineLayout = dev.CreatePipelineLayout(&plDesc);

    printf("PipelineManager: initialized with 4 bind group layouts\n");
}

wgpu::ShaderModule PipelineManager::GetOrCreateShader(uint32_t shaderType) {
    auto it = mShaderCache.find(shaderType);
    if (it != mShaderCache.end()) return it->second;

    // For Tier 1, all shader types use the standard shader
    const char* src = kStandardShaderSource;

    wgpu::ShaderSourceWGSL wgslSource;
    wgslSource.code = src;

    wgpu::ShaderModuleDescriptor desc{};
    desc.label = "StandardShader";
    desc.nextInChain = &wgslSource;
    wgpu::ShaderModule module = mDevice->Device().CreateShaderModule(&desc);

    mShaderCache[shaderType] = module;
    return module;
}

bool PipelineManager::ReloadShaders() {
    // Try to find the shader source file relative to the build directory.
    // Build dir is native/build/, source is native/src/gfx/standard_wgsl.inc
    const char* paths[] = {
        "../src/gfx/standard_wgsl.inc",          // from native/build/
        "native/src/gfx/standard_wgsl.inc",      // from repo root
        "src/gfx/standard_wgsl.inc",             // from native/
    };

    FILE* fp = nullptr;
    const char* usedPath = nullptr;
    for (auto* path : paths) {
        fp = fopen(path, "rb");
        if (fp) { usedPath = path; break; }
    }
    if (!fp) {
        printf("ReloadShaders: could not find standard_wgsl.inc\n");
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // The .inc file is a C string literal (starts with R"(...) and ends with )").
    // Read it raw, then strip the R"( prefix and )" suffix to get pure WGSL.
    std::string raw(size, '\0');
    fread(&raw[0], 1, size, fp);
    fclose(fp);

    // Strip C raw-string delimiters: R"DELIM( ... )DELIM"
    // Find the opening R"...( and closing )..."
    size_t rQuote = raw.find("R\"");
    if (rQuote == std::string::npos) {
        printf("ReloadShaders: no R\" found in %s\n", usedPath);
        return false;
    }
    size_t openParen = raw.find('(', rQuote + 2);
    if (openParen == std::string::npos) {
        printf("ReloadShaders: no opening ( found in %s\n", usedPath);
        return false;
    }
    // The delimiter is the text between R" and (
    std::string delim = raw.substr(rQuote + 2, openParen - (rQuote + 2));
    // Closing is )DELIM"
    std::string closeTag = ")" + delim + "\"";
    size_t end = raw.rfind(closeTag);
    if (end == std::string::npos || end <= openParen) {
        printf("ReloadShaders: no closing %s found in %s\n", closeTag.c_str(), usedPath);
        return false;
    }
    sLiveShaderSource = raw.substr(openParen + 1, end - (openParen + 1));

    // Test-compile the shader before committing.
    // Dawn always returns a non-null module even on error, so we must use
    // GetCompilationInfo to detect failures before flushing caches.
    wgpu::ShaderSourceWGSL wgslSource;
    wgslSource.code = sLiveShaderSource.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.label = "ReloadTest";
    desc.nextInChain = &wgslSource;
    wgpu::ShaderModule testModule = mDevice->Device().CreateShaderModule(&desc);

    bool hasError = false;
    wgpu::Future future = testModule.GetCompilationInfo(
        wgpu::CallbackMode::WaitAnyOnly,
        [&hasError](wgpu::CompilationInfoRequestStatus status,
                    wgpu::CompilationInfo const* info) {
            if (!info) return;
            for (size_t i = 0; i < info->messageCount; i++) {
                auto& msg = info->messages[i];
                const char* severity = "info";
                if (msg.type == wgpu::CompilationMessageType::Error) {
                    severity = "ERROR";
                    hasError = true;
                } else if (msg.type == wgpu::CompilationMessageType::Warning) {
                    severity = "warning";
                }
                printf("ReloadShaders: %s (line %llu): %.*s\n",
                       severity, (unsigned long long)msg.lineNum,
                       (int)msg.message.length, msg.message.data);
            }
        });
    mDevice->Instance().WaitAny(future, UINT64_MAX);

    if (hasError) {
        printf("ReloadShaders: compilation failed, keeping old shaders\n");
        sLiveShaderSource.clear();
        return false;
    }

    // Success — swap the active source and flush caches
    kStandardShaderSource = sLiveShaderSource.c_str();
    mShaderCache.clear();
    mPipelineCache.clear();

    printf("ReloadShaders: reloaded from %s (%zu bytes), caches cleared\n",
           usedPath, sLiveShaderSource.size());
    return true;
}

wgpu::BlendState PipelineManager::MapBlend(WgpuBlend blend) {
    wgpu::BlendState bs{};
    auto& color = bs.color;
    auto& alpha = bs.alpha;

    // Initialize to valid defaults — Dawn rejects Undefined (0) as invalid.
    color.operation = wgpu::BlendOperation::Add;
    color.srcFactor = wgpu::BlendFactor::One;
    color.dstFactor = wgpu::BlendFactor::Zero;
    alpha.operation = wgpu::BlendOperation::Add;
    alpha.srcFactor = wgpu::BlendFactor::One;
    alpha.dstFactor = wgpu::BlendFactor::Zero;

    switch (blend) {
    case WgpuBlend::Dest:
        color.srcFactor = wgpu::BlendFactor::Zero;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::Src:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::Zero;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::Add:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::SrcAlpha:
        color.srcFactor = wgpu::BlendFactor::SrcAlpha;
        color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::SrcAlphaAdd:
        color.srcFactor = wgpu::BlendFactor::SrcAlpha;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::Subtract:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::ReverseSubtract;
        break;
    case WgpuBlend::Multiply:
        color.srcFactor = wgpu::BlendFactor::Dst;
        color.dstFactor = wgpu::BlendFactor::Zero;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::PreMultAlpha:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::Screen:
        color.srcFactor = wgpu::BlendFactor::OneMinusDst;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Add;
        break;
    case WgpuBlend::Lighten:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Max;
        break;
    case WgpuBlend::Darken:
        color.srcFactor = wgpu::BlendFactor::One;
        color.dstFactor = wgpu::BlendFactor::One;
        color.operation = wgpu::BlendOperation::Min;
        break;
    default:
        // Fallback: use Src blend (opaque)
        break;
    }

    alpha.srcFactor = color.srcFactor;
    alpha.dstFactor = color.dstFactor;
    alpha.operation = color.operation;

    return bs;
}

wgpu::DepthStencilState PipelineManager::MapDepthStencil(WgpuZMode z, WgpuStencil s) {
    wgpu::DepthStencilState ds{};
    ds.format = wgpu::TextureFormat::Depth24PlusStencil8;

    switch (z) {
    case WgpuZMode::Disable:
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::Always;
        break;
    case WgpuZMode::Normal:
        ds.depthWriteEnabled = wgpu::OptionalBool::True;
        ds.depthCompare = wgpu::CompareFunction::Less;
        break;
    case WgpuZMode::Transparent:
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::LessEqual;
        break;
    case WgpuZMode::Force:
        ds.depthWriteEnabled = wgpu::OptionalBool::True;
        ds.depthCompare = wgpu::CompareFunction::Always;
        break;
    case WgpuZMode::Decal:
        ds.depthWriteEnabled = wgpu::OptionalBool::True;
        ds.depthCompare = wgpu::CompareFunction::LessEqual;
        break;
    }

    // Stencil (Tier 1: basic support)
    if (s == WgpuStencil::Write) {
        ds.stencilFront.compare = wgpu::CompareFunction::Always;
        ds.stencilFront.passOp = wgpu::StencilOperation::Replace;
        ds.stencilBack = ds.stencilFront;
    } else if (s == WgpuStencil::Test) {
        ds.stencilFront.compare = wgpu::CompareFunction::Equal;
        ds.stencilBack = ds.stencilFront;
    }

    return ds;
}

wgpu::CullMode PipelineManager::MapCull(WgpuCull cull) {
    switch (cull) {
    case WgpuCull::None:      return wgpu::CullMode::None;
    case WgpuCull::Regular:   return wgpu::CullMode::Back;
    case WgpuCull::Backwards: return wgpu::CullMode::Front;
    default:                  return wgpu::CullMode::None;
    }
}

wgpu::RenderPipeline PipelineManager::CreatePipeline(const PipelineKey& key) {
    wgpu::ShaderModule shader = GetOrCreateShader(key.shaderType);

    // Vertex layout
    const wgpu::VertexBufferLayout* vtxLayout;
    if (key.layout == VertexLayoutType::Skinned) {
        vtxLayout = &VertexFormats::SkinnedLayout();
    } else {
        vtxLayout = &VertexFormats::StaticLayout();
    }

    // Blend state
    wgpu::BlendState blendState = MapBlend(key.blend);

    wgpu::ColorTargetState colorTarget{};
    colorTarget.format = key.targetFormat;
    colorTarget.blend = &blendState;
    colorTarget.writeMask = key.alphaWrite
        ? wgpu::ColorWriteMask::All
        : (wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue);

    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs_main";
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    wgpu::DepthStencilState ds{};
    if (key.hasDepth) {
        ds = MapDepthStencil(key.zMode, key.stencil);
        if (key.depthBias != 0) {
            ds.depthBias = key.depthBias;
            ds.depthBiasSlopeScale = 0.0f;
            ds.depthBiasClamp = 0.0f;
        }
    }

    wgpu::RenderPipelineDescriptor pipeDesc{};
    pipeDesc.label = (key.layout == VertexLayoutType::Skinned) ? "MainSkinned" : "MainStatic";
    pipeDesc.layout = mPipelineLayout;
    pipeDesc.vertex.module = shader;
    pipeDesc.vertex.entryPoint = (key.layout == VertexLayoutType::Skinned) ? "vs_skinned" : "vs_main";
    pipeDesc.vertex.bufferCount = 1;
    pipeDesc.vertex.buffers = vtxLayout;
    pipeDesc.fragment = &fragment;
    pipeDesc.depthStencil = key.hasDepth ? &ds : nullptr;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeDesc.primitive.frontFace = wgpu::FrontFace::CCW; // D3D LH CW front → WebGPU RH CCW front
    pipeDesc.primitive.cullMode = MapCull(key.cull);
    pipeDesc.multisample.count = key.sampleCount;
    // WebGPU spec: alphaToCoverageEnabled requires count > 1
    pipeDesc.multisample.alphaToCoverageEnabled = key.alphaToCoverage && key.sampleCount > 1;

    static bool sLog = getenv("MILO_DEBUG_PIPELINES") != nullptr;
    if (sLog) {
        printf(
            "DC3 Pipeline: label=%s fmt=%d samples=%u depth=%d shader=%u alphaCut=%d alphaWrite=%d\n",
            key.layout == VertexLayoutType::Skinned ? "MainSkinned" : "MainStatic",
            (int)key.targetFormat,
            key.sampleCount,
            key.hasDepth ? 1 : 0,
            key.shaderType,
            key.alphaCut ? 1 : 0,
            key.alphaWrite ? 1 : 0
        );
    }

    return mDevice->Device().CreateRenderPipeline(&pipeDesc);
}

int PipelineManager::PreWarm(wgpu::TextureFormat mainFmt, wgpu::TextureFormat rtFmt) {
    // Enumerate the exact draw-time key space the RB3 backend's DrawMesh can
    // request (Rnd_Wgpu_RB3.cpp:4435-4486). Every field that varies at draw
    // time is swept here; every field that is *constant* at draw time is set to
    // the same constant, so each warmed key is bit-for-bit identical to a key
    // GetPipeline() will later look up (cache-key match is the whole point —
    // a mismatched warm entry would still leave the real draw paying the
    // compile). Constant draw-time fields: shaderType=0, cull=None,
    // stencil=Ignore, sampleCount=1, alphaToCoverage=false, depthBias=0.
    //
    // Varying draw-time fields swept here:
    //   blend     : material GetBlend() clamped to 0..7 (default Src(1),
    //               gemForce→Src(1)).
    //   zMode     : material GetZMode() clamped 0..4 (default Normal(1),
    //               text/gem→Disable(0)).
    //   layout    : Static / Skinned (skinned-vertex meshes).
    //   alphaCut  : material mAlphaCut bool.
    //   pass      : main pass (mainFmt, hasDepth=true, alphaWrite=false) OR
    //               RT pass (rtFmt, hasDepth=false, alphaWrite=true, sky-dome).
    //
    // Main pass: 8(blend) x 5(zMode) x 2(layout) x 2(alphaCut) = 160 keys —
    // a complete superset of every main-pass key the nav was ever observed to
    // request (boot->hub->song_select->gameplay: 22 distinct main-pass keys,
    // spanning blend 0..7 / zMode 0..4 / both layouts / both alphaCut). Swept
    // fully (not the observed 22) so a venue/material this profiling run did not
    // exercise still finds its pipeline warm.
    //
    // RT pass (sky-dome render-target: rtFmt, no depth, alphaWrite): swept
    // STATIC-layout only (40 keys). The RT target is backdrop-only static
    // geometry — skinned meshes (characters) never render into it, confirmed by
    // the key census (the lone observed RT key is blend=3/zMode=2/layout=Static,
    // and it lands inside the splash venue-build burst, so it MUST be pre-warmed
    // too). Skipping the 40 skinned-RT combos trims dead compiles with no risk.
    //
    // 160 main + 40 RT = 200 distinct keys when mainFmt != rtFmt (web: BGRA8
    // surface vs RGBA8 RT); when mainFmt == rtFmt (native headless readback) the
    // RT-pass keys still differ from main only by hasDepth/alphaWrite, so all
    // three passes stay distinct (240 entries). Either way every key is a
    // superset of the real draw set. All share one shader module
    // (GetOrCreateShader caches it). The compile cost is paid HERE, during the
    // idle boot/intro/splash dwell (async-I/O-bound, with slack), instead of
    // synchronously on the splash->main_hub venue-build frame. On native this is
    // a ~0.7 s synchronous burst absorbed by boot (no recorded frame spikes); on
    // web CreateRenderPipeline is async so it is a ~4 ms dispatch that warms the
    // pipelines off-thread before the venue frame draws.
    BuildPreWarmKeys(mainFmt, rtFmt);
    int created = 0;
    for (const PipelineKey& key : mPreWarmKeys) {
        if (mPipelineCache.find(key) == mPipelineCache.end()) {
            mPipelineCache[key] = CreatePipeline(key);
            ++created;
        }
    }
    mPreWarmCursor = (int)mPreWarmKeys.size();  // PreWarm == fully warmed
    return created;
}

// Enumerate the full pre-warm key space (the exact superset documented above) into
// mPreWarmKeys, once. Shared by PreWarm (synchronous) and PreWarmStep (chunked) so
// both warm the identical key set.
void PipelineManager::BuildPreWarmKeys(wgpu::TextureFormat mainFmt,
                                       wgpu::TextureFormat rtFmt) {
    if (mPreWarmStarted) return;
    mPreWarmStarted = true;
    struct PassVariant { wgpu::TextureFormat fmt; bool hasDepth; bool alphaWrite; bool skinned; };
    const PassVariant passes[3] = {
        { mainFmt, true,  false, false },  // main pass, static
        { mainFmt, true,  false, true  },  // main pass, skinned (characters)
        { rtFmt,   false, true,  false },  // RT pass, static only (sky-dome)
    };
    mPreWarmKeys.reserve(3 * 8 * 5 * 2);
    for (const auto& pass : passes) {
        for (int blend = 0; blend <= 7; ++blend) {
            for (int zMode = 0; zMode <= 4; ++zMode) {
                for (int alphaCutI = 0; alphaCutI <= 1; ++alphaCutI) {
                    PipelineKey key{};
                    key.shaderType = 0;
                    key.blend = (WgpuBlend)blend;
                    key.zMode = (WgpuZMode)zMode;
                    key.cull = WgpuCull::None;
                    key.stencil = WgpuStencil::Ignore;
                    key.layout = pass.skinned ? VertexLayoutType::Skinned
                                              : VertexLayoutType::Static;
                    key.targetFormat = pass.fmt;
                    key.sampleCount = 1;
                    key.hasDepth = pass.hasDepth;
                    key.alphaCut = (alphaCutI != 0);
                    key.alphaWrite = pass.alphaWrite;
                    key.alphaToCoverage = false;
                    key.depthBias = 0;
                    mPreWarmKeys.push_back(key);
                }
            }
        }
    }
}

// Chunked pre-warm — see header. Creates at most `maxThisCall` pipelines from
// mPreWarmCursor (the COUNT is the primary limiter — see header on why a wall
// budget can't bound the async web compile); the optional budgetMs is a native
// safety. Always makes ≥1 key of forward progress. The whole 240-key set warms
// over ceil(240/maxThisCall) frames instead of one ~590 ms blocking flush.
int PipelineManager::PreWarmStep(wgpu::TextureFormat mainFmt,
                                 wgpu::TextureFormat rtFmt,
                                 int maxThisCall, float budgetMs) {
    BuildPreWarmKeys(mainFmt, rtFmt);
    const int total = (int)mPreWarmKeys.size();
    if (mPreWarmCursor >= total) return 0;
    if (maxThisCall < 1) maxThisCall = 1;

    double t0 = FrameTraceNowMs();
    int madeThisCall = 0;
    while (mPreWarmCursor < total && madeThisCall < maxThisCall) {
        const PipelineKey& key = mPreWarmKeys[mPreWarmCursor];
        if (mPipelineCache.find(key) == mPipelineCache.end())
            mPipelineCache[key] = CreatePipeline(key);
        ++mPreWarmCursor;
        ++madeThisCall;
        // Native safety budget (inert on web: client create is ~0 ms).
        if (budgetMs > 0.f && (FrameTraceNowMs() - t0) >= (double)budgetMs)
            break;
    }
    return total - mPreWarmCursor;  // remaining
}

wgpu::RenderPipeline PipelineManager::GetPipeline(const PipelineKey& key) {
    auto it = mPipelineCache.find(key);
    if (it != mPipelineCache.end()) return it->second;

    // Frame-trace: only the cache MISS pays the (expensive) shader/pipeline
    // compile; the hit path above is free and stays uninstrumented.
    double ftStart = gFrameTraceActive ? FrameTraceNowMs() : 0.0;
    wgpu::RenderPipeline pipeline = CreatePipeline(key);
    if (gFrameTraceActive) {
        gPipelineCreateMsThisFrame += (float)(FrameTraceNowMs() - ftStart);
        gPipelineCreateCountThisFrame++;
    }
    mPipelineCache[key] = pipeline;

    if (mPipelineCache.size() == 512) {
        fprintf(stderr, "PipelineManager: warning — cache reached 512 entries, possible leak\n");
    }
    return pipeline;
}
