#include "gfx/ShadowPass.h"
#include "gfx/GpuDevice.h"
#include "gfx/VertexFormats.h"
#include "platform/Rnd_Wgpu.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/Lit.h"
#include "rndobj/Mat.h"
#include "rndobj/Mesh.h"
#include "obj/Dir.h"

#include <cmath>
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
    texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
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
    RndEnviron* env = RndEnviron::Current();
    if (env) {
        ObjPtrList<RndLight>& lights = env->LightsApprox();
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
}
