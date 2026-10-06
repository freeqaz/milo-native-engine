// DC3 Native Port — Particle Billboard Rendering
// Generates camera-facing quads for each active particle and batch-draws them.

#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "rndobj/Part.h"
#include "rndobj/Cam.h"
#include "rndobj/Mat.h"
#include "platform/rndshape/RndShape.h"

#include <cstring>
#include <vector>

extern WgpuRnd* gWgpuRnd;

// Simple particle vertex: position + UV + color
struct ParticleVertex {
    float pos[3];
    float uv[2];
    float color[4];
};

// Shared dynamic vertex/index buffers for particle rendering.
//
// The vertex buffer is a per-frame arena: each system's quads are appended at
// sParticleVBUsed and drawn from that offset. Queue::WriteBuffer runs at the
// next Submit, ahead of every command in the frame encoder, so writing each
// system's quads to offset 0 left every particle draw of the frame reading the
// LAST system's vertices (the title screen's rooftop fire showed up inside the
// cloud render target and on every other emitter).
static wgpu::Buffer sParticleVB;
static wgpu::Buffer sParticleIB;
static int sParticleVBCapacity = 0;  // in vertices
static int sParticleVBUsed = 0;      // vertices appended this frame
static int sParticleVBFrame = -1;    // WgpuRnd::FrameID() the arena belongs to
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

// Pipeline-overridable (RB3 shape only; both 0 for DC3):
//  kGammaColor  the particle colour is a gamma-space value, as RB3 retail
//               multiplies it into the (gamma) texture; decode it to match the
//               decoded texture (see standard_wgsl.inc, gammaShading).
//  kMaskScale   >0: write the pseudo-HDR bloom mask, alpha = luma(rgb) * this,
//               as retail's particle shader does for an AllowHDR material.
override kGammaColor: f32 = 0.0;
override kMaskScale: f32 = 0.0;

fn decodeSrgb(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((max(c, vec3f(0.0)) + 0.055) / 1.055, vec3f(2.4));
    return select(lo, hi, c > vec3f(0.04045));
}

@fragment fn fs_particle(in: VOut) -> @location(0) vec4f {
    let tex = textureSample(particleTex, particleSampler, in.uv);
    var col = in.color;
    if (kGammaColor > 0.5) {
        col = vec4f(decodeSrgb(col.rgb), col.a);
    }
    var c = tex * col;
    if (c.a < 0.004) { discard; }
    if (kMaskScale > 0.0) {
        c.a = clamp(dot(c.rgb, vec3f(0.3, 0.59, 0.11)) * kMaskScale, 0.0, 1.0);
    }
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

// Reserve room for maxParticles quads in this frame's vertex arena and return
// the first vertex index. When the arena is full a larger buffer replaces it;
// draws already recorded keep the old buffer alive through the encoder.
static int EnsureBuffers(int maxParticles) {
    int neededVerts = maxParticles * 4;
    int neededIndices = maxParticles * 6;

    int frame = gWgpuRnd->FrameID();
    if (frame != sParticleVBFrame) {
        sParticleVBFrame = frame;
        sParticleVBUsed = 0;
    }
    if (sParticleVBUsed + neededVerts > sParticleVBCapacity) {
        int cap = sParticleVBCapacity * 2;
        if (cap < sParticleVBUsed + neededVerts) cap = sParticleVBUsed + neededVerts;
        if (cap < 4096) cap = 4096;
        wgpu::BufferDescriptor desc{};
        desc.size = (uint64_t)cap * sizeof(ParticleVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        sParticleVB = gWgpuRnd->Gpu().Device().CreateBuffer(&desc);
        sParticleVBCapacity = cap;
        sParticleVBUsed = 0;
    }
    int first = sParticleVBUsed;
    sParticleVBUsed += neededVerts;

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
    return first;
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

    // Material tint and haze (rndshape::kPartMaterialTint; RB3-Wii only).
    float mcr = 1.0f, mcg = 1.0f, mcb = 1.0f, mca = 1.0f;
    bool isHaze = false;
    if constexpr (rndshape::kPartMaterialTint) {
        const Hmx::Color& mc = mat->GetColor();
        mcr = mc.red; mcg = mc.green; mcb = mc.blue; mca = mc.alpha;
        if (mca < 0.999f) {
            isHaze = true;
            mca *= 0.35f;
        }
    }

    // UV tiling
    int tilesAcross = rndshape::PartTilesAcross(sys);
    int tilesDown = rndshape::PartTilesDown(sys);
    if (tilesAcross < 1) tilesAcross = 1;
    if (tilesDown < 1) tilesDown = 1;
    float tileW = 1.0f / tilesAcross;
    float tileH = 1.0f / tilesDown;

    // Count particles and generate vertices
    std::vector<ParticleVertex> verts;
    verts.reserve(256);

    for (RndParticle* p = head; p; p = p->next) {
        float size = p->size * 0.5f;
        const Vector3 wp = rndshape::PartWorldPos(sys, p);
        float cx = wp.x, cy = wp.y, cz = wp.z;

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
        int tileIdx = rndshape::PartTileIndex(p);
        float u0 = (tileIdx % tilesAcross) * tileW;
        float v0 = (tileIdx / tilesAcross) * tileH;
        float u1 = u0 + tileW;
        float v1 = v0 + tileH;

        float cr = p->col.red * mcr, cg = p->col.green * mcg, cb = p->col.blue * mcb,
              ca = p->col.alpha * mca;
        if (isHaze) {
            // Fade a haze sprite out as it nears the camera: full alpha once its
            // centre is two half-sizes ahead, none at or behind the eye.
            float ahead = (cx - camXfm.v.x) * camXfm.m.y.x + (cy - camXfm.v.y) * camXfm.m.y.y +
                          (cz - camXfm.v.z) * camXfm.m.y.z;
            float t = ahead / ((size > 1.0f ? size : 1.0f) * 2.0f);
            ca *= t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

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

    // Project with the camera that is current now (a particle system can be the
    // first draw after a camera change, e.g. inside a render-target pass).
    gWgpuRnd->EnsureSceneUniformsCurrent();

    EnsureParticlePipeline();
    int firstVert = EnsureBuffers(numParticles);

    auto& dev = gWgpuRnd->Gpu().Device();
    auto& queue = gWgpuRnd->Gpu().Queue();
    auto& pass = gWgpuRnd->CurrentPass();

    // Upload vertex data
    uint64_t vbOffset = (uint64_t)firstVert * sizeof(ParticleVertex);
    queue.WriteBuffer(sParticleVB, vbOffset, verts.data(), verts.size() * sizeof(ParticleVertex));

    // Create pipeline for this blend mode
    wgpu::BlendState bs = gWgpuRnd->Pipelines().MapBlend((WgpuBlend)mat->GetBlend());

    wgpu::ColorTargetState ct{};
    ct.format = gWgpuRnd->CurrentTargetFormat();
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;
    // RB3's retail post chain reads the frame's alpha as its bloom mask: in the
    // main frame a particle writes the mask (AllowHDR material) or leaves alpha
    // alone. Both are no-ops for DC3 (rndshape::RetailBloomMaskActive false).
    float maskScale = 0.0f;
    if (rndshape::RetailBloomMaskActive() && gWgpuRnd->CurrentPassHasDepth() &&
        !gWgpuRnd->ActiveTargetTex()) {
        maskScale = rndshape::BloomMaskScale(rndshape::Mat(mat));
        if (maskScale <= 0.0f)
            ct.writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                           wgpu::ColorWriteMask::Blue;
    }
    wgpu::ConstantEntry consts[2] = {};
    consts[0].key = "kGammaColor";
    consts[0].value = rndshape::kGammaSpaceShading ? 1.0 : 0.0;
    consts[1].key = "kMaskScale";
    consts[1].value = maskScale;

    wgpu::FragmentState frag{};
    frag.module = sParticleShader;
    frag.entryPoint = "fs_particle";
    frag.constantCount = 2;
    frag.constants = consts;
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
    if (RndTex* diffTex = mat->GetDiffuseTex()) {
        // Upload on first use, as MaterialSetup does for meshes: a texture only
        // a particle system samples is otherwise never uploaded and the quads
        // draw untextured (solid, alpha-less).
        diffTex->PresyncBitmap();
        texView = GetGpuTexView(diffTex);
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
    pass.SetVertexBuffer(0, sParticleVB, vbOffset, verts.size() * sizeof(ParticleVertex));
    pass.SetIndexBuffer(sParticleIB, wgpu::IndexFormat::Uint16, 0,
                        numParticles * 6 * sizeof(uint16_t));
    pass.DrawIndexed(numParticles * 6);
}

void PartTerminate() {
    sParticleVB = nullptr;
    sParticleIB = nullptr;
    sParticleVBCapacity = 0;
    sParticleVBUsed = 0;
    sParticleVBFrame = -1;
    sParticleIBCapacity = 0;
    sParticleShader = nullptr;
    sParticleBGL = nullptr;
    sParticlePipelineLayout = nullptr;
}
