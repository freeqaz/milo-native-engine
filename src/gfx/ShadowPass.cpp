#include "gfx/ShadowPass.h"
#include "gfx/GpuDevice.h"
#include "gfx/UniformRingBuffer.h"
#include "gfx/VertexFormats.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/BoneSetup.h"
#include "platform/MeshGpuCache.h"
#include "platform/TransformUtils.h"
#include "gfx/UniformStructs.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/Mat.h"
#include "rndobj/Mesh.h"
#include "obj/Dir.h"
#include "platform/rndshape/RndShape.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include <cstdio>
#include <cstring>

static const char* kShadowShaderSource = R"WGSL(
struct LightVP {
    matrix: mat4x4f,
};
@group(0) @binding(0) var<uniform> lightVP: LightVP;

struct ObjectUB {
    world: mat4x4f,
    worldInvTranspose: mat4x4f,
};
@group(1) @binding(0) var<uniform> object: ObjectUB;

@vertex fn vs_shadow(@location(0) pos: vec3f) -> @builtin(position) vec4f {
    return lightVP.matrix * object.world * vec4f(pos, 1.0);
}

// Skinned variant
struct BoneUB {
    bones: array<mat4x4f, 40>,
};
@group(2) @binding(0) var<uniform> bones: BoneUB;

struct SkinInput {
    @location(0) pos: vec3f,
    @location(4) boneWeights: vec4f,
    @location(5) boneIndices: vec4u,
};

@vertex fn vs_shadow_skinned(in: SkinInput) -> @builtin(position) vec4f {
    var skinnedPos = vec4f(0.0);
    let p = vec4f(in.pos, 1.0);
    for (var i = 0u; i < 4u; i++) {
        let w = in.boneWeights[i];
        if (w > 0.0) {
            skinnedPos += bones.bones[in.boneIndices[i]] * p * w;
        }
    }
    skinnedPos.w = 1.0;
    return lightVP.matrix * object.world * skinnedPos;
}
)WGSL";

static void BuildOrthoMatrix(float left, float right, float bottom, float top,
                              float near, float far, float* out) {
    memset(out, 0, 64);
    out[0]  = 2.0f / (right - left);
    out[5]  = 2.0f / (top - bottom);
    out[10] = 1.0f / (far - near);
    out[12] = -(right + left) / (right - left);
    out[13] = -(top + bottom) / (top - bottom);
    out[14] = -near / (far - near);
    out[15] = 1.0f;
}

static void BuildLookAtMatrix(const float* eye, const float* at, const float* up, float* out) {
    float f[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    float flen = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    if (flen > 0) { f[0]/=flen; f[1]/=flen; f[2]/=flen; }

    float r[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] };
    float rlen = sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]);
    if (rlen > 0) { r[0]/=rlen; r[1]/=rlen; r[2]/=rlen; }

    float u[3] = { r[1]*f[2]-r[2]*f[1], r[2]*f[0]-r[0]*f[2], r[0]*f[1]-r[1]*f[0] };

    memset(out, 0, 64);
    out[0] = r[0]; out[1] = u[0]; out[2]  = -f[0];
    out[4] = r[1]; out[5] = u[1]; out[6]  = -f[1];
    out[8] = r[2]; out[9] = u[2]; out[10] = -f[2];
    out[12] = -(r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2]);
    out[13] = -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    out[14] =  (f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    out[15] = 1.0f;
}

static void MultiplyMatrix4x4(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i*4+j] = 0;
            for (int k = 0; k < 4; k++) {
                out[i*4+j] += a[i*4+k] * b[k*4+j];
            }
        }
    }
}

extern void DrawMeshShadow(RndMesh* mesh);

static bool IsShadowTransparentBlend(int blend) {
    return blend == BaseMaterial::kBlendSrcAlpha ||
           blend == BaseMaterial::kBlendSrcAlphaAdd ||
           blend == BaseMaterial::kBlendAdd ||
           blend == BaseMaterial::kBlendSubtract ||
           blend == BaseMaterial::kPreMultAlpha;
}

void ShadowPass::Init(GpuDevice& gpu) {
    EnsurePipelines(gpu);
}

void ShadowPass::EnsurePipelines(GpuDevice& gpu) {
    if (mShadowReady) return;
    auto& dev = gpu.Device();

    // Shadow depth texture
    wgpu::TextureDescriptor texDesc{};
    texDesc.label = "ShadowDepth";
    texDesc.size = {kShadowMapSize, kShadowMapSize, 1};
    texDesc.format = wgpu::TextureFormat::Depth32Float;
    // CopySrc: lets a test read the caster map back (ShadowPass::DepthTexture).
    texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                    wgpu::TextureUsage::CopySrc;
    texDesc.mipLevelCount = 1;
    mShadowDepthTex = dev.CreateTexture(&texDesc);
    mShadowDepthView = mShadowDepthTex.CreateView();

    // Comparison sampler for shadow mapping
    wgpu::SamplerDescriptor sampDesc{};
    sampDesc.label = "ShadowCompSampler";
    sampDesc.compare = wgpu::CompareFunction::LessEqual;
    sampDesc.magFilter = wgpu::FilterMode::Linear;
    sampDesc.minFilter = wgpu::FilterMode::Linear;
    sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
    sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
    mShadowSampler = dev.CreateSampler(&sampDesc);

    // Shader module
    wgpu::ShaderSourceWGSL src;
    src.code = kShadowShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.label = "ShadowShader";
    smDesc.nextInChain = &src;
    mShadowShader = dev.CreateShaderModule(&smDesc);

    // Light VP uniform buffer
    wgpu::BufferDescriptor bufDesc{};
    bufDesc.label = "ShadowLightVP";
    bufDesc.size = 64;
    bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mShadowLightVPBuffer = dev.CreateBuffer(&bufDesc);

    // Group 0: lightVP uniform
    wgpu::BindGroupLayoutEntry sceneEntry{};
    sceneEntry.binding = 0;
    sceneEntry.visibility = wgpu::ShaderStage::Vertex;
    sceneEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    sceneEntry.buffer.minBindingSize = 64;

    wgpu::BindGroupLayoutDescriptor sceneBglDesc{};
    sceneBglDesc.entryCount = 1;
    sceneBglDesc.entries = &sceneEntry;
    mShadowSceneBGL = dev.CreateBindGroupLayout(&sceneBglDesc);

    // Group 1: object world
    wgpu::BindGroupLayoutEntry objEntry{};
    objEntry.binding = 0;
    objEntry.visibility = wgpu::ShaderStage::Vertex;
    objEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    objEntry.buffer.minBindingSize = 0;

    wgpu::BindGroupLayoutDescriptor objBglDesc{};
    objBglDesc.entryCount = 1;
    objBglDesc.entries = &objEntry;
    mShadowObjectBGL = dev.CreateBindGroupLayout(&objBglDesc);

    // Group 2: bones
    wgpu::BindGroupLayoutEntry boneEntry{};
    boneEntry.binding = 0;
    boneEntry.visibility = wgpu::ShaderStage::Vertex;
    boneEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    boneEntry.buffer.minBindingSize = 0;

    wgpu::BindGroupLayoutDescriptor boneBglDesc{};
    boneBglDesc.entryCount = 1;
    boneBglDesc.entries = &boneEntry;
    mShadowBoneBGL = dev.CreateBindGroupLayout(&boneBglDesc);

    // Pipeline layouts
    {
        wgpu::BindGroupLayout layouts[2] = {mShadowSceneBGL, mShadowObjectBGL};
        wgpu::PipelineLayoutDescriptor plDesc{};
        plDesc.bindGroupLayoutCount = 2;
        plDesc.bindGroupLayouts = layouts;
        mShadowPipelineLayout = dev.CreatePipelineLayout(&plDesc);
    }
    {
        wgpu::BindGroupLayout layouts[3] = {mShadowSceneBGL, mShadowObjectBGL, mShadowBoneBGL};
        wgpu::PipelineLayoutDescriptor plDesc{};
        plDesc.bindGroupLayoutCount = 3;
        plDesc.bindGroupLayouts = layouts;
        mShadowSkinnedPipelineLayout = dev.CreatePipelineLayout(&plDesc);
    }

    // Depth-stencil state
    wgpu::DepthStencilState ds{};
    ds.format = wgpu::TextureFormat::Depth32Float;
    ds.depthWriteEnabled = wgpu::OptionalBool::True;
    ds.depthCompare = wgpu::CompareFunction::Less;
    ds.depthBias = 2;
    ds.depthBiasSlopeScale = 1.5f;

    // Static shadow pipeline
    {
        const wgpu::VertexBufferLayout* vtxLayout = &VertexFormats::StaticLayout();
        wgpu::RenderPipelineDescriptor pipeDesc{};
        pipeDesc.label = "ShadowStatic";
        pipeDesc.layout = mShadowPipelineLayout;
        pipeDesc.vertex.module = mShadowShader;
        pipeDesc.vertex.entryPoint = "vs_shadow";
        pipeDesc.vertex.bufferCount = 1;
        pipeDesc.vertex.buffers = vtxLayout;
        pipeDesc.depthStencil = &ds;
        pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipeDesc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipeDesc.primitive.cullMode = wgpu::CullMode::Back;
        mShadowStaticPipeline = dev.CreateRenderPipeline(&pipeDesc);
    }

    // Skinned shadow pipeline
    {
        const wgpu::VertexBufferLayout* vtxLayout = &VertexFormats::SkinnedLayout();
        wgpu::RenderPipelineDescriptor pipeDesc{};
        pipeDesc.label = "ShadowSkinned";
        pipeDesc.layout = mShadowSkinnedPipelineLayout;
        pipeDesc.vertex.module = mShadowShader;
        pipeDesc.vertex.entryPoint = "vs_shadow_skinned";
        pipeDesc.vertex.bufferCount = 1;
        pipeDesc.vertex.buffers = vtxLayout;
        pipeDesc.depthStencil = &ds;
        pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipeDesc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipeDesc.primitive.cullMode = wgpu::CullMode::Back;
        mShadowSkinnedPipeline = dev.CreateRenderPipeline(&pipeDesc);
    }

    printf("ShadowPass: pipelines initialized (%dx%d depth texture)\n",
           kShadowMapSize, kShadowMapSize);
    mShadowReady = true;
}

void ShadowPass::Render(wgpu::CommandEncoder& encoder, UniformRingBuffer& objectRing,
                        UniformRingBuffer& boneRing, GpuDevice& gpu) {
    EnsurePipelines(gpu);

    // Get the primary light direction
    float lightDir[3] = {0, -1, 0};
    RndEnviron* env = rndshape::CurrentEnv();
    if (env) {
        ObjPtrList<RndLight>& lights = rndshape::EnvLightsApprox(env);
        for (ObjPtrList<RndLight>::iterator it = lights.begin(); it != lights.end(); ++it) {
            RndLight* light = *it;
            if (light && light->GetType() == RndLight::kDirectional) {
                Transform xfm = light->WorldXfm();
                lightDir[0] = -xfm.m.z.x;
                lightDir[1] = -xfm.m.z.y;
                lightDir[2] = -xfm.m.z.z;
                break;
            }
        }
    }

    // Build light view-projection matrix
    float lightDist = 20.0f;
    float eye[3] = { -lightDir[0]*lightDist, -lightDir[1]*lightDist, -lightDir[2]*lightDist };
    float at[3] = {0, 0, 0};
    float up[3] = {0, 1, 0};
    if (fabsf(lightDir[1]) > 0.9f) { up[0] = 0; up[1] = 0; up[2] = 1; }

    float viewMat[16], projMat[16];
    BuildLookAtMatrix(eye, at, up, viewMat);
    BuildOrthoMatrix(-10, 10, -10, 10, 0.1f, 50.0f, projMat);
    MultiplyMatrix4x4(projMat, viewMat, mLightViewProj);

    // Upload light VP
    gpu.Queue().WriteBuffer(mShadowLightVPBuffer, 0, mLightViewProj, 64);

    // Create scene bind group
    wgpu::BindGroupEntry sceneEntry{};
    sceneEntry.binding = 0;
    sceneEntry.buffer = mShadowLightVPBuffer;
    sceneEntry.size = 64;
    wgpu::BindGroupDescriptor sceneBgDesc{};
    sceneBgDesc.layout = mShadowSceneBGL;
    sceneBgDesc.entryCount = 1;
    sceneBgDesc.entries = &sceneEntry;
    mShadowSceneBindGroup = gpu.Device().CreateBindGroup(&sceneBgDesc);

    // Begin shadow depth render pass
    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mShadowDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear;
    depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "ShadowPass";
    rpDesc.depthStencilAttachment = &depthAtt;

    mShadowPass = encoder.BeginRenderPass(&rpDesc);
    mShadowPass.SetBindGroup(0, mShadowSceneBindGroup);
    mInShadowPass = true;

    // Draw all opaque meshes into shadow map
    ObjectDir* worldDir = nullptr;
    if (env) worldDir = env->Dir();
    if (!worldDir) {
        RndCam* cam = RndCam::Current();
        if (cam) worldDir = cam->Dir();
    }
    if (worldDir) {
        ObjDirItr<RndMesh> meshItr(worldDir, true);
        for (; meshItr; ++meshItr) {
            RndMesh* mesh = meshItr;
            if (!mesh->Showing()) continue;
            if (strstr(mesh->Name(), "_lod")) continue;
            RndMat* mat = mesh->Mat();
            if (mat && !IsShadowTransparentBlend(mat->GetBlend())) {
                DrawMeshShadow(mesh);
            }
        }
    }

    mShadowPass.End();
    mShadowPass = nullptr;
    mInShadowPass = false;
    mShadowAvailable = true;
}

// ---------------------------------------------------------------------------
// The DC3 projected-shadow caster pass, self-contained.
//
// It used to draw each caster through Mesh_Wgpu's DrawMeshShadow (the generic
// pass's per-mesh path), which per mesh wrote the object ring, created an
// object bind group, and for a skinned mesh wrote the bone ring and created a
// bone bind group: 20 of the pass's ~100 perf samples were CreateBindGroup.
// Here the layouts take a dynamic offset, every caster's world matrix (and
// bone palette) goes into this pass's own two buffers in ONE WriteBuffer each,
// and the bind groups are created once, again only when a buffer has to grow.
// Same shader, same vertex layouts, same depth state as the generic pipelines,
// so the map is the same; nothing here touches Mesh_Wgpu, MaterialSetup or
// the shared object/bone rings.
// ---------------------------------------------------------------------------
namespace {
constexpr uint64_t kCasterObjectSize = 128;   // ObjectUB: world + worldInvTranspose
constexpr uint64_t kCasterObjectStride = 256; // minUniformBufferOffsetAlignment
constexpr uint64_t kCasterBoneSize = sizeof(BoneUniforms);   // 2560, a multiple of 256
constexpr uint64_t kCasterClipStride = 256;   // one light's clip transform (64 bytes used)
constexpr int kCasterTiles = 4;                // 2x2 atlas over the depth texture
static_assert(kCasterBoneSize % 256 == 0, "bone palette stride must stay 256-aligned");
static_assert(offsetof(ObjectUniforms, worldInvTranspose) == 64,
              "the caster shader reads ObjectUniforms' first 128 bytes");
}

void ShadowPass::EnsureCasterPipelines(GpuDevice& gpu) {
    if (mCasterReady) return;
    auto& dev = gpu.Device();

    // Group 0: one light's clip transform per atlas tile, at a dynamic offset.
    wgpu::BindGroupLayoutEntry sceneEntry{};
    sceneEntry.binding = 0;
    sceneEntry.visibility = wgpu::ShaderStage::Vertex;
    sceneEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    sceneEntry.buffer.hasDynamicOffset = true;
    sceneEntry.buffer.minBindingSize = 64;
    wgpu::BindGroupLayoutDescriptor sceneDesc{};
    sceneDesc.label = "CasterSceneBGL";
    sceneDesc.entryCount = 1;
    sceneDesc.entries = &sceneEntry;
    mCasterSceneBGL = dev.CreateBindGroupLayout(&sceneDesc);

    wgpu::BindGroupLayoutEntry objEntry{};
    objEntry.binding = 0;
    objEntry.visibility = wgpu::ShaderStage::Vertex;
    objEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    objEntry.buffer.hasDynamicOffset = true;
    objEntry.buffer.minBindingSize = kCasterObjectSize;
    wgpu::BindGroupLayoutDescriptor objDesc{};
    objDesc.label = "CasterObjectBGL";
    objDesc.entryCount = 1;
    objDesc.entries = &objEntry;
    mCasterObjectBGL = dev.CreateBindGroupLayout(&objDesc);

    wgpu::BindGroupLayoutEntry boneEntry = objEntry;
    boneEntry.buffer.minBindingSize = kCasterBoneSize;
    wgpu::BindGroupLayoutDescriptor boneDesc{};
    boneDesc.label = "CasterBoneBGL";
    boneDesc.entryCount = 1;
    boneDesc.entries = &boneEntry;
    mCasterBoneBGL = dev.CreateBindGroupLayout(&boneDesc);

    wgpu::PipelineLayout staticLayout, skinnedLayout;
    {
        wgpu::BindGroupLayout layouts[2] = {mCasterSceneBGL, mCasterObjectBGL};
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 2;
        d.bindGroupLayouts = layouts;
        staticLayout = dev.CreatePipelineLayout(&d);
    }
    {
        wgpu::BindGroupLayout layouts[3] = {mCasterSceneBGL, mCasterObjectBGL, mCasterBoneBGL};
        wgpu::PipelineLayoutDescriptor d{};
        d.bindGroupLayoutCount = 3;
        d.bindGroupLayouts = layouts;
        skinnedLayout = dev.CreatePipelineLayout(&d);
    }

    wgpu::DepthStencilState ds{};   // as EnsurePipelines
    ds.format = wgpu::TextureFormat::Depth32Float;
    ds.depthWriteEnabled = wgpu::OptionalBool::True;
    ds.depthCompare = wgpu::CompareFunction::Less;
    ds.depthBias = 2;
    ds.depthBiasSlopeScale = 1.5f;
    for (int skinned = 0; skinned < 2; skinned++) {
        wgpu::RenderPipelineDescriptor pd{};
        pd.label = skinned ? "CasterSkinned" : "CasterStatic";
        pd.layout = skinned ? skinnedLayout : staticLayout;
        pd.vertex.module = mShadowShader;
        pd.vertex.entryPoint = skinned ? "vs_shadow_skinned" : "vs_shadow";
        pd.vertex.bufferCount = 1;
        pd.vertex.buffers = skinned ? &VertexFormats::SkinnedLayout() : &VertexFormats::StaticLayout();
        pd.depthStencil = &ds;
        pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pd.primitive.frontFace = wgpu::FrontFace::CCW;
        pd.primitive.cullMode = wgpu::CullMode::Back;
        (skinned ? mCasterSkinnedPipeline : mCasterStaticPipeline) = dev.CreateRenderPipeline(&pd);
    }

    wgpu::BufferDescriptor clipDesc{};
    clipDesc.label = "CasterClips";
    clipDesc.size = kCasterClipStride * kCasterTiles;
    clipDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mCasterClipBuf = dev.CreateBuffer(&clipDesc);
    wgpu::BindGroupEntry clipEntry{};
    clipEntry.binding = 0;
    clipEntry.buffer = mCasterClipBuf;
    clipEntry.size = 64;
    wgpu::BindGroupDescriptor sceneBgDesc{};
    sceneBgDesc.layout = mCasterSceneBGL;
    sceneBgDesc.entryCount = 1;
    sceneBgDesc.entries = &clipEntry;
    mCasterSceneBindGroup = dev.CreateBindGroup(&sceneBgDesc);
    mCasterStats.bindGroupsCreated++;
    mCasterReady = true;
}

// Grow `buf` to hold `need` bytes (doubling), and rebuild its bind group.
void ShadowPass::EnsureCasterBuffer(GpuDevice& gpu, wgpu::Buffer& buf, uint64_t& capacity,
                                    wgpu::BindGroup& group, const wgpu::BindGroupLayout& layout,
                                    uint64_t bindingSize, uint64_t need, const char* label) {
    if (buf && capacity >= need) return;
    uint64_t cap = capacity ? capacity : bindingSize * 16;
    while (cap < need) cap *= 2;
    wgpu::BufferDescriptor bd{};
    bd.label = label;
    bd.size = cap;
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    buf = gpu.Device().CreateBuffer(&bd);
    capacity = cap;

    wgpu::BindGroupEntry e{};
    e.binding = 0;
    e.buffer = buf;
    e.offset = 0;
    e.size = bindingSize;
    wgpu::BindGroupDescriptor gd{};
    gd.layout = layout;
    gd.entryCount = 1;
    gd.entries = &e;
    group = gpu.Device().CreateBindGroup(&gd);
    mCasterStats.bindGroupsCreated++;
}

void ShadowPass::RenderCasters(wgpu::CommandEncoder& encoder, GpuDevice& gpu,
                               const CasterSet* sets, int numSets) {
    EnsurePipelines(gpu);
    if (!mShadowReady) return;
    EnsureCasterPipelines(gpu);

    // Gather: one 256-byte object slot per drawn caster, one bone palette per
    // skinned caster, all on the CPU, then one upload each.
    struct CasterDraw {
        GpuMeshData* data;
        uint32_t objOffset;
        uint32_t boneOffset;
    };
    struct TileDraws {
        int tile;
        uint32_t clipOffset;
        size_t first, count;
    };
    std::vector<CasterDraw> draws;
    std::vector<TileDraws> tiles;
    float clips[kCasterTiles][kCasterClipStride / sizeof(float)] = {};
    mCasterObjectStaging.clear();
    mCasterBoneStaging.clear();
    for (int si = 0; si < numSets; si++) {
        const CasterSet& set = sets[si];
        if (set.tile < 0 || set.tile >= kCasterTiles || !set.clipFromWorld) continue;
        memcpy(clips[set.tile], set.clipFromWorld, 64);
        TileDraws td{set.tile, (uint32_t)(set.tile * kCasterClipStride), draws.size(), 0};
        for (int i = 0; i < set.numMeshes; i++) {
            RndMesh* mesh = set.meshes[i];
            if (!mesh || !EnsureMeshUploaded(mesh)) continue;
            GpuMeshData* data = GetMeshGpuData(mesh);
            if (!data || !data->vertexBuffer || !data->indexBuffer || data->numIndices == 0) continue;

            ObjectUniforms obj{};
            if (data->skinned) {
                Transform identity;   // the palette carries the world, as DrawMeshShadow
                identity.Reset();
                FillObjectUniforms(identity, obj);
            } else {
                FillObjectUniforms(mesh->WorldXfm(), obj);
            }
            CasterDraw d{data, (uint32_t)mCasterObjectStaging.size(), 0};
            mCasterObjectStaging.resize(mCasterObjectStaging.size() + kCasterObjectStride, 0);
            memcpy(mCasterObjectStaging.data() + d.objOffset, &obj, kCasterObjectSize);
            if (data->skinned) {
                BoneUniforms bones{};
                FillBoneUniforms(mesh, bones);
                d.boneOffset = (uint32_t)mCasterBoneStaging.size();
                mCasterBoneStaging.resize(mCasterBoneStaging.size() + kCasterBoneSize);
                memcpy(mCasterBoneStaging.data() + d.boneOffset, &bones, kCasterBoneSize);
            }
            draws.push_back(d);
        }
        td.count = draws.size() - td.first;
        tiles.push_back(td);
    }

    EnsureCasterBuffer(gpu, mCasterObjectBuf, mCasterObjectCap, mCasterObjectBindGroup,
                       mCasterObjectBGL, kCasterObjectSize,
                       std::max<uint64_t>(mCasterObjectStaging.size(), kCasterObjectStride),
                       "CasterObjects");
    EnsureCasterBuffer(gpu, mCasterBoneBuf, mCasterBoneCap, mCasterBoneBindGroup,
                       mCasterBoneBGL, kCasterBoneSize,
                       std::max<uint64_t>(mCasterBoneStaging.size(), kCasterBoneSize),
                       "CasterBones");
    gpu.Queue().WriteBuffer(mCasterClipBuf, 0, clips, sizeof(clips));
    if (!mCasterObjectStaging.empty())
        gpu.Queue().WriteBuffer(mCasterObjectBuf, 0, mCasterObjectStaging.data(),
                                mCasterObjectStaging.size());
    if (!mCasterBoneStaging.empty())
        gpu.Queue().WriteBuffer(mCasterBoneBuf, 0, mCasterBoneStaging.data(),
                                mCasterBoneStaging.size());

    // One pass over the whole texture: every tile clears to 1, and each light
    // draws through a viewport on its own tile (its clip -1..1 fills the tile).
    wgpu::RenderPassDepthStencilAttachment depthAtt{};
    depthAtt.view = mShadowDepthView;
    depthAtt.depthLoadOp = wgpu::LoadOp::Clear;
    depthAtt.depthStoreOp = wgpu::StoreOp::Store;
    depthAtt.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.label = "ProjShadowCasters";
    rpDesc.depthStencilAttachment = &depthAtt;

    const float tileSize = kShadowMapSize / 2.0f;
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
    for (const TileDraws& td : tiles) {
        pass.SetViewport((td.tile % 2) * tileSize, (td.tile / 2) * tileSize, tileSize, tileSize,
                         0.0f, 1.0f);
        pass.SetBindGroup(0, mCasterSceneBindGroup, 1, &td.clipOffset);
        int bound = -1;   // 0 static, 1 skinned
        for (size_t k = td.first; k < td.first + td.count; k++) {
            const CasterDraw& d = draws[k];
            const int skinned = d.data->skinned ? 1 : 0;
            if (skinned != bound) {
                pass.SetPipeline(skinned ? mCasterSkinnedPipeline : mCasterStaticPipeline);
                bound = skinned;
            }
            pass.SetBindGroup(1, mCasterObjectBindGroup, 1, &d.objOffset);
            if (skinned) pass.SetBindGroup(2, mCasterBoneBindGroup, 1, &d.boneOffset);
            const size_t vertexSize = skinned ? sizeof(GpuVertexSkinned) : sizeof(GpuVertex);
            pass.SetVertexBuffer(0, d.data->vertexBuffer, 0, d.data->numVertices * vertexSize);
            pass.SetIndexBuffer(d.data->indexBuffer, wgpu::IndexFormat::Uint16, 0,
                                d.data->numIndices * sizeof(uint16_t));
            pass.DrawIndexed(d.data->numIndices);
        }
    }
    pass.End();
    mCasterStats.frames++;
    mCasterStats.lastDraws = (uint32_t)draws.size();
    mCasterStats.lastTiles = (uint32_t)tiles.size();
    mCasterStats.lastUploadBytes =
        mCasterObjectStaging.size() + mCasterBoneStaging.size() + sizeof(clips);
}

void ShadowPass::Terminate() {
    mShadowDepthTex = nullptr;
    mShadowDepthView = nullptr;
    mShadowSampler = nullptr;
    mShadowShader = nullptr;
    mShadowSceneBGL = nullptr;
    mShadowObjectBGL = nullptr;
    mShadowBoneBGL = nullptr;
    mShadowPipelineLayout = nullptr;
    mShadowSkinnedPipelineLayout = nullptr;
    mShadowStaticPipeline = nullptr;
    mShadowSkinnedPipeline = nullptr;
    mShadowLightVPBuffer = nullptr;
    mShadowSceneBindGroup = nullptr;
    mShadowReady = false;
    mCasterObjectBGL = nullptr;
    mCasterBoneBGL = nullptr;
    mCasterStaticPipeline = nullptr;
    mCasterSkinnedPipeline = nullptr;
    mCasterSceneBindGroup = nullptr;
    mCasterSceneBGL = nullptr;
    mCasterClipBuf = nullptr;
    mCasterObjectBuf = nullptr;
    mCasterBoneBuf = nullptr;
    mCasterObjectBindGroup = nullptr;
    mCasterBoneBindGroup = nullptr;
    mCasterObjectCap = mCasterBoneCap = 0;
    mCasterReady = false;
}

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* ShadowPassWgslSource() { return kShadowShaderSource; }
