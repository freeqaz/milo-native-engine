// DC3 Native Port — Particle Billboard Rendering
// Generates camera-facing quads for each active particle and batch-draws them.

#include "platform/Part_Wgpu.h"
#include "gfx/FrameCapture.h"
#include "platform/Rnd_Wgpu.h"
#include "platform/TexGpu.h"
#include "rndobj/Part.h"
#include "rndobj/Cam.h"
#include "rndobj/Mat.h"
#include "platform/rndshape/RndShape.h"

#include "gfx/PipelineCount.h"
#include "gfx/RenderPipelineCache.h"
#include "platform/MeshPipelineKey.h"

#include <cmath>
#include <cstring>
#include <vector>

extern WgpuRnd* gWgpuRnd;

// Simple particle vertex: position + UV + color
struct ParticleVertex {
    float pos[3];
    float uv[2];
    float color[4];
    float mask;  // bloom mask scale, the same for the whole system (fs_particle)
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
// Pipelines by blend/target/depth/samples/mask (gfx/RenderPipelineCache.h).
static RenderPipelineCache sParticlePipelines;

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
    @location(3) mask: f32,
};

struct VOut {
    @builtin(position) clip: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
    @location(2) @interpolate(flat) mask: f32,
};

@vertex fn vs_particle(in: VIn) -> VOut {
    var out: VOut;
    out.clip = scene.viewProj * vec4f(in.pos, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.mask = in.mask;
    return out;
}

// The particle colour and the texel are multiplied as stored and written as
// is, which is RB3 retail's particles.ps (gamma space, like the standard
// shader under material.gammaShading).
// mask > 0: write the pseudo-HDR bloom mask, alpha = luma(rgb) * mask, as
// retail's particle shader does for an AllowHDR material. The scale is the
// frame's post-proc's, so it rides in the vertices rather than in the
// pipeline: a pipeline constant made every post-proc a new pipeline, compiled
// at the draw.

@fragment fn fs_particle(in: VOut) -> @location(0) vec4f {
    let tex = textureSample(particleTex, particleSampler, in.uv);
    var c = tex * in.color;
    if (c.a < 0.004) { discard; }
    if (in.mask > 0.0) {
        c.a = clamp(dot(c.rgb, vec3f(0.3, 0.59, 0.11)) * in.mask, 0.0, 1.0);
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

// The pipeline a particle system with material `mat` drawn with `blend` asks
// for in a pass (the draw's, or the load-time warm's: WarmParticlePipelines).
// The bloom mask scale a particle system drawn with `mat` writes in this pass
// (0: none).
static float ParticleMaskScale(RndMat* mat, bool hasDepth, bool toTexture) {
    if (!rndshape::RetailBloomMaskActive() || !hasDepth || toTexture) return 0.0f;
    // DC3's scale is negative (pre-clamp luma in the mesh shader); a
    // particle's colour is unlit, so its magnitude is applied as is.
    return std::fabs(rndshape::BloomMaskScale(rndshape::Mat(mat)));
}

// `maskWrite`: this draw writes the bloom mask (ParticleMaskScale > 0; the
// scale itself is in the vertices).
static wgpu::RenderPipeline ParticlePipeline(RndMat* mat, WgpuBlend blend, bool maskWrite,
                                             wgpu::TextureFormat format, bool hasDepth,
                                             bool toTexture, uint32_t samples) {
    EnsureParticlePipeline();
    wgpu::BlendState bs = gWgpuRnd->Pipelines().MapBlend(blend);

    wgpu::ColorTargetState ct{};
    ct.format = format;
    ct.blend = &bs;
    ct.writeMask = wgpu::ColorWriteMask::All;
    // The retail post chain reads the frame's alpha as its bloom mask (DC3's
    // retail chain, RB3's): in the main frame a particle writes the mask
    // (AllowHDR material) or leaves alpha alone.
    (void)mat;
    if (rndshape::RetailBloomMaskActive() && hasDepth && !toTexture && !maskWrite)
        ct.writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
                       wgpu::ColorWriteMask::Blue;

    wgpu::FragmentState frag{};
    frag.module = sParticleShader;
    frag.entryPoint = "fs_particle";
    frag.targetCount = 1;
    frag.targets = &ct;

    wgpu::VertexAttribute attrs[4] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x3; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = wgpu::VertexFormat::Float32x4; attrs[2].offset = 20; attrs[2].shaderLocation = 2;
    attrs[3].format = wgpu::VertexFormat::Float32; attrs[3].offset = 36; attrs[3].shaderLocation = 3;

    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(ParticleVertex);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 4;
    vbl.attributes = attrs;

    wgpu::DepthStencilState ds{};
    if (hasDepth) {
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
    pipeDesc.depthStencil = hasDepth ? &ds : nullptr;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeDesc.primitive.cullMode = wgpu::CullMode::None;
    pipeDesc.multisample.count = samples;

    // Cached (gfx/RenderPipelineCache.h): a per-draw CreateRenderPipeline
    // recompiled the particle shader for every system, every frame.
    return sParticlePipelines.GetOrCreate(
        gWgpuRnd->Gpu().Device(),
        RenderPipelineCache::MakeKey(bs, ct.format, ct.writeMask, hasDepth,
                                     pipeDesc.multisample.count, 0.0f, 0u),
        pipeDesc);
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

Hmx::Color ParticleShaderColor(RndMat* mat, RndEnviron* env) {
    // c0: the material colour, premultiplied for kPreMultAlpha (blend 7 in
    // both shapes' Blend enums).
    Hmx::Color k = mat->GetColor();
    if ((int)mat->GetBlend() == 7) {
        k.red *= k.alpha;
        k.green *= k.alpha;
        k.blue *= k.alpha;
    }
    // c1: the environ ambient, alpha included, when the material uses it.
    if (rndshape::MatUseEnviron(mat) && env && rndshape::EnvHasAmbientFogOwner(env)) {
        const Hmx::Color& a = env->AmbientColor();
        k.red *= a.red;
        k.green *= a.green;
        k.blue *= a.blue;
        k.alpha *= a.alpha;
    }
    // c5.y: intensify doubles the texel, all four channels.
    if (rndshape::MatIntensify(mat)) {
        k.red *= 2.0f;
        k.green *= 2.0f;
        k.blue *= 2.0f;
        k.alpha *= 2.0f;
    }
    return k;
}

ParticleQuadAxes ParticleBillboardAxes(const float pos[3], const float camRight[3],
                                       const float camUp[3], float halfSize, float angle,
                                       bool retailXbox, float screenAspect, float swingArm) {
    ParticleQuadAxes q;
    const float rx = camRight[0], ry = camRight[1], rz = camRight[2];
    const float ux = camUp[0], uy = camUp[1], uz = camUp[2];
    const float size = halfSize;
    float cx = pos[0], cy = pos[1], cz = pos[2];
    float srx = rx * size, sry = ry * size, srz = rz * size;
    float sux = ux * size, suy = uy * size, suz = uz * size;
    if (retailXbox) {
        // The shipped particle vertex shader (rndshape::PartBillboard): up is
        // scaled by the screen aspect, the angle turns the quad clockwise, and
        // the swing arm moves it along its turned up axis.
        const float cosA = cosf(angle), sinA = sinf(angle);
        const float S = screenAspect;
        const float rotRx = rx * cosA - ux * S * sinA, rotRy = ry * cosA - uy * S * sinA,
                    rotRz = rz * cosA - uz * S * sinA;
        const float rotUx = rx * sinA + ux * S * cosA, rotUy = ry * sinA + uy * S * cosA,
                    rotUz = rz * sinA + uz * S * cosA;
        srx = rotRx * size; sry = rotRy * size; srz = rotRz * size;
        sux = rotUx * size; suy = rotUy * size; suz = rotUz * size;
        cx += rotUx * swingArm; cy += rotUy * swingArm; cz += rotUz * swingArm;
    } else if (angle != 0.0f) {
        // The engine's original billboard (RB3-Wii, and DC3-shape consumers
        // without RndParticleSys::ScreenAspect()).
        float cosA = cosf(angle);
        float sinA = sinf(angle);
        float nrx = srx * cosA + sux * sinA;
        float nry = sry * cosA + suy * sinA;
        float nrz = srz * cosA + suz * sinA;
        float nux = -srx * sinA + sux * cosA;
        float nuy = -sry * sinA + suy * cosA;
        float nuz = -srz * sinA + suz * cosA;
        srx = nrx; sry = nry; srz = nrz;
        sux = nux; suy = nuy; suz = nuz;
    }
    q.centre[0] = cx; q.centre[1] = cy; q.centre[2] = cz;
    q.right[0] = srx; q.right[1] = sry; q.right[2] = srz;
    q.up[0] = sux; q.up[1] = suy; q.up[2] = suz;
    return q;
}

// The aligned path of the shipped particles vertex shader (dc3 xbox_shaders,
// type "particles", microcode from instruction 44 to label L17; the same in
// every permutation). Its inputs, from DxParticleSys::DrawShowing and
// DrawParticles: the vertex carries size and 2 * vel (local space), c47 =
// 0.5 * camera X and c48 = 0.5 * S * camera Z (S = screen aspect, both
// expressed in local space through Invert(mRelativeXfm.m)), c49 = (aligned,
// stretched, constantArea, 2 * stretch_scale). For the corner whose texture
// coords are (u, v), with sigma = 2v - 1 across and tau = 2u - 1 along:
//
//   a = 2V.c47 = V.X_local     b = 2V.c48 = S * V.Z_local
//   len = |(a, b)|              (A, B) = (a, b) / len, or (a, b) when len = 0
//   no perspective (c47.w = 1):
//     L = stretched ? size + 2 * stretch_scale * len : size
//     W = constantArea ? (L > 0.01 ? size^2 / L : 0.01 * size) : size
//     corner = P + (1 - tau) / 2 * L * (A X + B S Z) + sigma * W / 2 * (B X - A S Z)
//   perspective (c47.w = 0):
//     W as above with size + len for L
//     corner = P + (1 - tau) * 2 * stretch_scale * V_world + sigma * W / 2 * (B X - A S Z)
//
// So the texture's top (tau = -1) leads along the velocity and its bottom sits
// on the particle, and a perspective streak is a world-space segment whose
// length does not include the particle size.
ParticleQuadAxes ParticleAlignedAxes(const float pos[3], const float velLocal[3],
                                     const float rel[9], const float camRight[3],
                                     const float camUp[3], float size,
                                     const ParticleAlignTerms& terms) {
    ParticleQuadAxes q;
    for (int i = 0; i < 3; i++) {
        q.centre[i] = pos[i];
        q.right[i] = 0.0f;
        q.up[i] = 0.0f;
    }
    const float* m = rel;
    // Cofactors of the relative matrix; X_local = X * Invert(m) (row vectors).
    const float cof[3][3] = {
        {m[4] * m[8] - m[5] * m[7], -(m[3] * m[8] - m[5] * m[6]), m[3] * m[7] - m[4] * m[6]},
        {-(m[1] * m[8] - m[2] * m[7]), m[0] * m[8] - m[2] * m[6], -(m[0] * m[7] - m[1] * m[6])},
        {m[1] * m[5] - m[2] * m[4], -(m[0] * m[5] - m[2] * m[3]), m[0] * m[4] - m[1] * m[3]},
    };
    const float det = m[0] * cof[0][0] + m[1] * cof[0][1] + m[2] * cof[0][2];
    if (det == 0.0f) return q;
    float xl[3], zl[3];
    for (int j = 0; j < 3; j++) {
        xl[j] = (camRight[0] * cof[j][0] + camRight[1] * cof[j][1] + camRight[2] * cof[j][2]) / det;
        zl[j] = (camUp[0] * cof[j][0] + camUp[1] * cof[j][1] + camUp[2] * cof[j][2]) / det;
    }
    const float S = terms.screenAspect;
    const float a = velLocal[0] * xl[0] + velLocal[1] * xl[1] + velLocal[2] * xl[2];
    const float b = S * (velLocal[0] * zl[0] + velLocal[1] * zl[1] + velLocal[2] * zl[2]);
    const float len = sqrtf(a * a + b * b);
    const float A = len > 0.0f ? a / len : a;
    const float B = len > 0.0f ? b / len : b;
    const float scale2 = 2.0f * terms.stretchScale;  // c49.w
    float along = terms.perspective ? size + len : size;
    if (!terms.perspective && terms.stretched) along = size + scale2 * len;
    float W = size;
    if (terms.constantArea) W = along > 0.01f ? size * size / along : 0.01f * size;
    for (int i = 0; i < 3; i++) {
        const float across = B * camRight[i] - A * S * camUp[i];
        q.right[i] = 0.5f * W * across;
        if (terms.perspective) {
            const float vw = velLocal[0] * m[i] + velLocal[1] * m[3 + i] + velLocal[2] * m[6 + i];
            q.up[i] = scale2 * vw;
        } else {
            q.up[i] = 0.5f * along * (A * camRight[i] + B * S * camUp[i]);
        }
        q.centre[i] = pos[i] + q.up[i];
    }
    return q;
}

ParticleUVRect ParticleTileUV(int tileIndex, int tilesAcross, int tilesDown, bool animateUVs) {
    ParticleUVRect r;
    if (!animateUVs) return r;  // the whole texture (c51.x == 0)
    if (tilesAcross < 1) tilesAcross = 1;
    if (tilesDown < 1) tilesDown = 1;
    const float tileW = 1.0f / tilesAcross;
    const float tileH = 1.0f / tilesDown;
    r.u0 = (tileIndex % tilesAcross) * tileW;
    r.v0 = (tileIndex / tilesAcross) * tileH;
    r.u1 = r.u0 + tileW;
    r.v1 = r.v0 + tileH;
    return r;
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

    // Material tint and haze (rndshape::kPartMaterialTint; RB3-Wii only), or
    // the retail Xbox particle shader's colour scale (ParticleShaderColor).
    float mcr = 1.0f, mcg = 1.0f, mcb = 1.0f, mca = 1.0f;
    bool isHaze = false;
    if constexpr (rndshape::kPartMaterialTint) {
        const Hmx::Color& mc = mat->GetColor();
        mcr = mc.red; mcg = mc.green; mcb = mc.blue; mca = mc.alpha;
        if (mca < 0.999f) {
            isHaze = true;
            mca *= 0.35f;
        }
    } else {
        Hmx::Color k = ParticleShaderColor(mat, rndshape::CurrentEnv());
        mcr = k.red; mcg = k.green; mcb = k.blue; mca = k.alpha;
    }

    const rndshape::PartBillboardTerms billboard = rndshape::PartBillboard(sys);
    const float camR[3] = {rx, ry, rz};
    const float camU[3] = {ux, uy, uz};
    ParticleAlignTerms alignTerms;
    alignTerms.screenAspect = billboard.screenAspect;
    alignTerms.stretched = billboard.stretched;
    alignTerms.constantArea = billboard.constantArea;
    alignTerms.perspective = billboard.perspective;
    alignTerms.stretchScale = billboard.stretchScale;

    // UV tiling: one tile of the sheet, or the whole texture when the system
    // does not animate its UVs (ParticleTileUV).
    const int tilesAcross = rndshape::PartTilesAcross(sys);
    const int tilesDown = rndshape::PartTilesDown(sys);
    const bool animateUVs = rndshape::PartAnimatesUVs(sys);

    // Count particles and generate vertices
    std::vector<ParticleVertex> verts;
    verts.reserve(256);
    const bool passHasDepth = gWgpuRnd->CurrentPassHasDepth();
    const bool passToTexture = gWgpuRnd->ActiveTargetTex() != nullptr;
    const float mask = ParticleMaskScale(mat, passHasDepth, passToTexture);

    for (RndParticle* p = head; p; p = p->next) {
        const Vector3 wp = rndshape::PartWorldPos(sys, p);
        const float pos[3] = {wp.x, wp.y, wp.z};
        ParticleQuadAxes q;
        if (billboard.velocityVerts) {
            const Vector3 v = rndshape::PartVelocity(p);
            const float vel[3] = {v.x, v.y, v.z};
            if (billboard.aligned) {
                q = ParticleAlignedAxes(pos, vel, billboard.rel, camR, camU, p->size,
                                        alignTerms);
            } else {
                // DrawParticles wrote 2 * vel where angle and swing arm go.
                q = ParticleBillboardAxes(pos, camR, camU, p->size * 0.5f, 2.0f * vel[0], true,
                                          billboard.screenAspect, 2.0f * vel[1]);
            }
        } else {
            q = ParticleBillboardAxes(
                pos, camR, camU, p->size * 0.5f, p->angle, billboard.retailXbox,
                billboard.screenAspect, billboard.retailXbox ? rndshape::PartSwingArm(p) : 0.0f);
        }
        const float size = p->size * 0.5f;  // the haze fade's scale
        const float cx = q.centre[0], cy = q.centre[1], cz = q.centre[2];
        const float srx = q.right[0], sry = q.right[1], srz = q.right[2];
        const float sux = q.up[0], suy = q.up[1], suz = q.up[2];

        // UV tile
        const ParticleUVRect uvr =
            ParticleTileUV(rndshape::PartTileIndex(p), tilesAcross, tilesDown, animateUVs);
        const float u0 = uvr.u0, v0 = uvr.v0, u1 = uvr.u1, v1 = uvr.v1;

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
        v.mask = mask;

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

    // Per-draw log (/api/capture/next, F12): particles are draws too, and the
    // colour recorded is the scale applied to every particle's own colour.
    if (FrameCapture::Get().IsCapturing()) {
        auto& rec = FrameCapture::Get().AddDraw();
        rec.meshName = sys->Name();
        rec.materialName = mat->Name();
        rec.cameraName = cam->Name();
        rec.blend = (int)mat->GetBlend();
        rec.zMode = (int)mat->GetZMode();
        rec.color[0] = mcr; rec.color[1] = mcg; rec.color[2] = mcb; rec.color[3] = mca;
        rec.alpha = mca;
        rec.useTexture = mat->GetDiffuseTex() ? 1.0f : 0.0f;
        rec.emissiveMultiplier = (float)numParticles;  // particle count
        const Vector3& wp = sys->WorldXfm().v;
        rec.worldPos[0] = wp.x; rec.worldPos[1] = wp.y; rec.worldPos[2] = wp.z;
    }

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

    wgpu::RenderPipeline pipe = ParticlePipeline(
        mat, (WgpuBlend)mat->GetBlend(), mask > 0.0f, gWgpuRnd->CurrentTargetFormat(),
        passHasDepth, passToTexture, gWgpuRnd->CurrentSampleCount());

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
    // Same view + sampler on the same layout is the same group: cached
    // (PLAN H-30), dropped with the texture through ForgetTextureView.
    wgpu::BindGroup texBG = gWgpuRnd->BindGroups().GetOrCreate(dev, bg1Desc);

    pass.SetPipeline(pipe);
    pass.SetBindGroup(0, sceneBG);
    pass.SetBindGroup(1, texBG);
    pass.SetVertexBuffer(0, sParticleVB, vbOffset, verts.size() * sizeof(ParticleVertex));
    pass.SetIndexBuffer(sParticleIB, wgpu::IndexFormat::Uint16, 0,
                        numParticles * 6 * sizeof(uint16_t));
    pass.DrawIndexed(numParticles * 6);
}

int WarmParticlePipelines(RndMat* mat, const MeshPassContext& ctx, const int* blends,
                          int numBlends) {
    // Not before the renderer's resources exist: EnsureParticlePipeline builds
    // its layout once, from the scene layout, and a load that finishes first
    // (the web brings its device up asynchronously) would latch a layout with
    // no group 0 -- every particle pipeline invalid for the rest of the run.
    if (!gWgpuRnd || !mat || !gWgpuRnd->GpuResourcesReady()) return 0;
    const uint64_t before = GpuPipelineCreateCount();
    // Whether the mask is written is the frame's (a current post-proc, no
    // environment fade), not the material's, so a blend that allows it is
    // warmed both ways.
    const bool frameMask = rndshape::RetailBloomMaskActive() && ctx.hasDepth && !ctx.toTexture;
    auto warm = [&](int blend) {
        ParticlePipeline(mat, (WgpuBlend)blend, false, ctx.targetFormat, ctx.hasDepth,
                         ctx.toTexture, ctx.sampleCount);
        if (frameMask && rndshape::AllowsBloomMaskFor(rndshape::Mat(mat), blend,
                                                      (bool)rndshape::Mat(mat)->GetAlphaWrite()))
            ParticlePipeline(mat, (WgpuBlend)blend, true, ctx.targetFormat, ctx.hasDepth,
                             ctx.toTexture, ctx.sampleCount);
    };
    warm((int)mat->GetBlend());
    for (int i = 0; i < numBlends; ++i) warm(blends[i]);
    return (int)(GpuPipelineCreateCount() - before);
}

void PartTerminate() {
    sParticlePipelines.Clear();
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

// Shipped WGSL accessor (gfx/ShippedWgsl.h): lets the validation test compile
// the exact source this file hands CreateShaderModule.
#include "gfx/ShippedWgsl.h"
const char* ParticleWgslSource() { return kParticleShaderSource; }
