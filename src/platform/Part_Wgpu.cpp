// DC3 Native Port — Particle Billboard Rendering
// Generates camera-facing quads for each active particle and batch-draws them.

#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "rndobj/Part.h"
#include "rndobj/Cam.h"
#include "rndobj/Mat.h"

#include <cstring>
#include <vector>

extern WgpuRnd* gWgpuRnd;

// Simple particle vertex: position + UV + color
struct ParticleVertex {
    float pos[3];
    float uv[2];
    float color[4];
};

// Shared dynamic vertex/index buffers for particle rendering
static wgpu::Buffer sParticleVB;
static wgpu::Buffer sParticleIB;
static int sParticleVBCapacity = 0;  // in vertices
static int sParticleIBCapacity = 0;  // in indices

// Particle pipeline state — reuses main renderer's SceneBGL at group 0
static wgpu::ShaderModule sParticleShader;
static wgpu::BindGroupLayout sParticleBGL;       // group 1: texture + sampler
static wgpu::PipelineLayout sParticlePipelineLayout;
static bool sParticlePipelineReady = false;

static const char* kParticleShaderSource = R"WGSL(
struct SceneUB {
    viewProj: mat4x4f,
};
@group(0) @binding(0) var<uniform> scene: SceneUB;

@group(1) @binding(0) var particleTex: texture_2d<f32>;
@group(1) @binding(1) var particleSampler: sampler;

struct VIn {
    @location(0) pos: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
};

struct VOut {
    @builtin(position) clip: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

@vertex fn vs_particle(in: VIn) -> VOut {
    var out: VOut;
    out.clip = scene.viewProj * vec4f(in.pos, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@fragment fn fs_particle(in: VOut) -> @location(0) vec4f {
    let tex = textureSample(particleTex, particleSampler, in.uv);
    let c = tex * in.color;
    if (c.a < 0.004) { discard; }
    return c;
}
)WGSL";

static void EnsureParticlePipeline() {
    if (sParticlePipelineReady) return;
    auto& dev = gWgpuRnd->Gpu().Device();

    // Shader
    wgpu::ShaderSourceWGSL src;
    src.code = kParticleShaderSource;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &src;
    sParticleShader = dev.CreateShaderModule(&smDesc);

    // Group 0: reuse main renderer's SceneBGL (shader only reads binding 0,
    // but unused BGL entries for shadow depth/sampler are allowed by WebGPU)
    wgpu::BindGroupLayout bgl0 = gWgpuRnd->Pipelines().SceneLayout();

    // Group 1: texture + sampler
    wgpu::BindGroupLayoutEntry e1[2] = {};
    e1[0].binding = 0;
    e1[0].visibility = wgpu::ShaderStage::Fragment;
    e1[0].texture.sampleType = wgpu::TextureSampleType::Float;
    e1[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    e1[1].binding = 1;
    e1[1].visibility = wgpu::ShaderStage::Fragment;
    e1[1].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bgl1Desc{};
    bgl1Desc.entryCount = 2;
    bgl1Desc.entries = e1;
    sParticleBGL = dev.CreateBindGroupLayout(&bgl1Desc);

    wgpu::BindGroupLayout layouts[2] = { bgl0, sParticleBGL };
    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 2;
    plDesc.bindGroupLayouts = layouts;
    sParticlePipelineLayout = dev.CreatePipelineLayout(&plDesc);

    sParticlePipelineReady = true;
}

static void EnsureBuffers(int maxParticles) {
    int neededVerts = maxParticles * 4;
    int neededIndices = maxParticles * 6;

    if (neededVerts > sParticleVBCapacity) {
        wgpu::BufferDescriptor desc{};
        desc.size = neededVerts * sizeof(ParticleVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        sParticleVB = gWgpuRnd->Gpu().Device().CreateBuffer(&desc);
        sParticleVBCapacity = neededVerts;
    }
    if (neededIndices > sParticleIBCapacity) {
        // Generate index data: 0,1,2, 2,1,3, 4,5,6, 6,5,7, ...
        std::vector<uint16_t> indices(neededIndices);
        for (int i = 0; i < maxParticles; i++) {
            int base = i * 4;
            int idx = i * 6;
            indices[idx + 0] = base + 0;
            indices[idx + 1] = base + 1;
            indices[idx + 2] = base + 2;
            indices[idx + 3] = base + 2;
            indices[idx + 4] = base + 1;
            indices[idx + 5] = base + 3;
        }
        wgpu::BufferDescriptor desc{};
        desc.size = neededIndices * sizeof(uint16_t);
        desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        sParticleIB = gWgpuRnd->Gpu().Device().CreateBuffer(&desc);
        gWgpuRnd->Gpu().Queue().WriteBuffer(sParticleIB, 0, indices.data(),
                                             neededIndices * sizeof(uint16_t));
        sParticleIBCapacity = neededIndices;
    }
}

void DrawParticlesBillboard(RndParticleSys* sys) {
    if (!gWgpuRnd || !gWgpuRnd->IsInPass()) return;

    RndParticle* head = sys->ActiveParticles();
    if (!head) return;

    RndMat* mat = sys->GetMat();
    if (!mat) return;

    // Get camera axes for billboarding
    RndCam* cam = RndCam::Current();
    if (!cam) return;

    const Transform& camXfm = cam->WorldXfm();
    // Camera right = X axis, camera up = Z axis (Milo convention)
    float rx = camXfm.m.x.x, ry = camXfm.m.x.y, rz = camXfm.m.x.z;
    float ux = camXfm.m.z.x, uy = camXfm.m.z.y, uz = camXfm.m.z.z;

    // UV tiling
    int tilesAcross = sys->NumTilesAcross();
    int tilesDown = sys->NumTilesDown();
    if (tilesAcross < 1) tilesAcross = 1;
    if (tilesDown < 1) tilesDown = 1;
    float tileW = 1.0f / tilesAcross;
    float tileH = 1.0f / tilesDown;

    // Count particles and generate vertices
    std::vector<ParticleVertex> verts;
    verts.reserve(256);

    for (RndParticle* p = head; p; p = p->next) {
        float size = p->size * 0.5f;
        float cx = p->pos.x, cy = p->pos.y, cz = p->pos.z;

        // Billboard offsets
        float srx = rx * size, sry = ry * size, srz = rz * size;
        float sux = ux * size, suy = uy * size, suz = uz * size;

        // Optional rotation by particle angle
        if (p->angle != 0.0f) {
            float cosA = cosf(p->angle);
            float sinA = sinf(p->angle);
            float nrx = srx * cosA + sux * sinA;
            float nry = sry * cosA + suy * sinA;
            float nrz = srz * cosA + suz * sinA;
            float nux = -srx * sinA + sux * cosA;
            float nuy = -sry * sinA + suy * cosA;
            float nuz = -srz * sinA + suz * cosA;
            srx = nrx; sry = nry; srz = nrz;
            sux = nux; suy = nuy; suz = nuz;
        }

        // UV tile
        int tileIdx = p->mCurrentTileIndex;
        float u0 = (tileIdx % tilesAcross) * tileW;
        float v0 = (tileIdx / tilesAcross) * tileH;
        float u1 = u0 + tileW;
        float v1 = v0 + tileH;

        float cr = p->col.red, cg = p->col.green, cb = p->col.blue, ca = p->col.alpha;

        // Quad: TL, BL, TR, BR
        ParticleVertex v;
        v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = ca;

        // TL: center - right + up
        v.pos[0] = cx - srx + sux; v.pos[1] = cy - sry + suy; v.pos[2] = cz - srz + suz;
        v.uv[0] = u0; v.uv[1] = v0;
        verts.push_back(v);

        // BL: center - right - up
        v.pos[0] = cx - srx - sux; v.pos[1] = cy - sry - suy; v.pos[2] = cz - srz - suz;
        v.uv[0] = u0; v.uv[1] = v1;
        verts.push_back(v);

        // TR: center + right + up
        v.pos[0] = cx + srx + sux; v.pos[1] = cy + sry + suy; v.pos[2] = cz + srz + suz;
        v.uv[0] = u1; v.uv[1] = v0;
        verts.push_back(v);

        // BR: center + right - up
        v.pos[0] = cx + srx - sux; v.pos[1] = cy + sry - suy; v.pos[2] = cz + srz - suz;
        v.uv[0] = u1; v.uv[1] = v1;
        verts.push_back(v);
    }

    int numParticles = (int)verts.size() / 4;
    if (numParticles == 0) return;

    EnsureParticlePipeline();
    EnsureBuffers(numParticles);

    auto& dev = gWgpuRnd->Gpu().Device();
    auto& queue = gWgpuRnd->Gpu().Queue();
    auto& pass = gWgpuRnd->CurrentPass();

    // Upload vertex data
    queue.WriteBuffer(sParticleVB, 0, verts.data(), verts.size() * sizeof(ParticleVertex));

    // Create pipeline for this blend mode
    wgpu::BlendState bs = gWgpuRnd->Pipelines().MapBlend((WgpuBlend)mat->GetBlend());

    wgpu::ColorTargetState ct{};
    ct.format = gWgpuRnd->CurrentTargetFormat();
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = sParticleShader;
    frag.entryPoint = "fs_particle";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[3] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x3; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 20; attrs[2].shaderLocation = 2;

    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(ParticleVertex);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 3;
    vbl.attributes = attrs;

    wgpu::DepthStencilState ds{};
    if (gWgpuRnd->CurrentPassHasDepth()) {
        ds.format = wgpu::TextureFormat::Depth24PlusStencil8;
        ds.depthWriteEnabled = wgpu::OptionalBool::False;
        ds.depthCompare = wgpu::CompareFunction::LessEqual;
    }

    wgpu::RenderPipelineDescriptor pipeDesc{};
    pipeDesc.layout = sParticlePipelineLayout;
    pipeDesc.vertex.module = sParticleShader;
    pipeDesc.vertex.entryPoint = "vs_particle";
    pipeDesc.vertex.bufferCount = 1;
    pipeDesc.vertex.buffers = &vbl;
    pipeDesc.fragment = &frag;
    pipeDesc.depthStencil = gWgpuRnd->CurrentPassHasDepth() ? &ds : nullptr;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeDesc.primitive.cullMode = wgpu::CullMode::None;
    pipeDesc.multisample.count = gWgpuRnd->CurrentSampleCount();

    // TODO: cache pipeline by blend mode
    wgpu::RenderPipeline pipe = dev.CreateRenderPipeline(&pipeDesc);

    // Bind group 0: reuse the main renderer's scene bind group (same SceneBGL)
    wgpu::BindGroup& sceneBG = gWgpuRnd->SceneBindGroup();

    // Bind group 1: texture + sampler
    wgpu::TextureView texView;
    if (mat->GetDiffuseTex()) {
        texView = GetGpuTexView(mat->GetDiffuseTex());
    }
    if (!texView) texView = gWgpuRnd->WhiteTexView();

    wgpu::BindGroupEntry texEntries[2] = {};
    texEntries[0].binding = 0;
    texEntries[0].textureView = texView;
    texEntries[1].binding = 1;
    texEntries[1].sampler = gWgpuRnd->DefaultSampler();

    wgpu::BindGroupDescriptor bg1Desc{};
    bg1Desc.layout = sParticleBGL;
    bg1Desc.entryCount = 2;
    bg1Desc.entries = texEntries;
    wgpu::BindGroup texBG = dev.CreateBindGroup(&bg1Desc);

    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, sceneBG);
    pass.SetBindGroup(1, texBG);
    pass.SetVertexBuffer(0, sParticleVB, 0, verts.size() * sizeof(ParticleVertex));
    pass.SetIndexBuffer(sParticleIB, wgpu::IndexFormat::Uint16, 0,
                        numParticles * 6 * sizeof(uint16_t));
    pass.DrawIndexed(numParticles * 6);
}

void PartTerminate() {
    sParticleVB = nullptr;
    sParticleIB = nullptr;
    sParticleShader = nullptr;
    sParticleBGL = nullptr;
    sParticlePipelineLayout = nullptr;
}
