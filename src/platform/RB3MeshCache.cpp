// RB3 per-mesh GPU vertex/index buffer upload cache — extracted from
// platform/Rnd_Wgpu_RB3.cpp (W1.2). See platform/RB3MeshCache.h for the design
// notes. RB3-only TU (MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3); never compiled
// alongside the DC3 MeshGpuCache (which also defines a CleanupGpuMesh).

#include "platform/RB3MeshCache.h"

#include "platform/Rnd_Wgpu_RB3.h"  // BandRnd (mGpu Device/Queue) + GpuVertexRB3 alias — for RB3EnsureMeshGpu
#include "rndobj/Mesh.h"

#include <cstring>

// The cache map (keyed by drawn mesh). Definition; declared extern in the header.
std::unordered_map<RndMesh*, RB3MeshEntry> sMeshGpu;

// The per-owner geom-generation counter. Definition; declared extern in the header.
std::unordered_map<RndMesh*, uint32_t> sGeomSyncGen;

// Drop a mesh's cached GPU buffers. Strong def displaces the weak no-op
// link-stub (native: rndobj_synth_link_stubs.s; web: missing_stubs.js). Called
// from RndMesh's HX_NATIVE destructor so freed meshes release their GPU buffers
// instead of leaking the cache slot for the lifetime of the process.
void CleanupGpuMesh(RndMesh* mesh) {
    sMeshGpu.erase(mesh);
    // A freed mesh may have been someone's geometry owner; drop its generation
    // counter too so the map doesn't grow unbounded. A recycled pointer re-Syncs
    // before its next draw (gen>=1 != any stamped 0), so this is safe.
    sGeomSyncGen.erase(mesh);
}

// --- Xbox 360 compressed vertex unpacking (36 bytes/vert, big-endian) ---
// Mirrors milo-native-engine gfx/VertexFormats.cpp UnpackCompressedVertices
// (which lives in the rndobj-coupled TU RB3 excludes). The D3D vertex decl is:
//   pos   = FLOAT3   POSITION  (3 BE floats, off 0)
//   color = D3DCOLOR COLOR     (packed, off 12)
//   uv    = FLOAT16_2 TEXCOORD (off 16)
//   norm  = DEC4N    NORMAL    (10-10-10-2, off 20)
//   tan   = DEC4N    TANGENT   (off 24); bone data off 28/32.
struct XboxCVert { int pos[3]; int color; int uv; int norm; int tan; int b0; int b1; };
static_assert(sizeof(XboxCVert) == 36, "XboxCVert must be 36 bytes");

static float BeFloat(int bits) {
    unsigned v = __builtin_bswap32((unsigned)bits); float f; std::memcpy(&f, &v, 4); return f;
}
static float Half2Float(unsigned short h) {
    unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    unsigned f;
    if (exp == 0) { if (mant == 0) f = sign << 31; else { float val = (float)mant / 1024.0f * (1.0f/16384.0f); return sign ? -val : val; } }
    else if (exp == 0x1F) f = (sign << 31) | 0x7F800000 | (mant << 13);
    else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float r; std::memcpy(&r, &f, 4); return r;
}
static void BeUV(int packed, float out[2]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = Half2Float((v >> 16) & 0xFFFF); out[1] = Half2Float(v & 0xFFFF);
}
static void BeColor(int packed, float out[4]) {
    // RB3's compressed vertex colour is a D3DCOLOR (D3DDECLTYPE_D3DCOLOR), ARGB
    // packed as 0xAARRGGBB. The asset is big-endian (Xbox 360): 4 bytes on disc
    // are [AA, RR, GG, BB]; read as a little-endian int then bswap32 restores
    // the natural 0xAARRGGBB, so R = (v>>16), G = (v>>8), B = (v>>0), A = (v>>24).
    // (The prior mapping read R from the low byte, swapping R<->B; this only
    // affected meshes that actually use vertex colour — i.e. PRELIT meshes,
    // since the shader now gates vertex-colour application on RndMat::mPreLit.)
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = ((v >> 16) & 0xFF) / 255.0f; // R
    out[1] = ((v >> 8)  & 0xFF) / 255.0f; // G
    out[2] = ((v >> 0)  & 0xFF) / 255.0f; // B
    out[3] = ((v >> 24) & 0xFF) / 255.0f; // A
}
static void BeDec4n(int packed, float out[3]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    int ix = (int)(v << 22) >> 22, iy = (int)(v << 12) >> 22, iz = (int)(v << 2) >> 22;
    out[0] = ix / 511.0f; out[1] = iy / 511.0f; out[2] = iz / 511.0f;
}
// UDEC4N: 10-10-10-2 UNSIGNED normalized (bone weights, BLENDWEIGHT slot).
static void BeUDec4n(int packed, float out[4]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = (v & 0x3FF) / 1023.0f;
    out[1] = ((v >> 10) & 0x3FF) / 1023.0f;
    out[2] = ((v >> 20) & 0x3FF) / 1023.0f;
    out[3] = ((v >> 30) & 0x3) / 3.0f;
}
// UBYTE4: four packed bone indices (BLENDINDICES slot). The blob is big-endian
// on disc; bswap restores Xbox byte order (idx0 in the low byte).
static void BeUByte4(int packed, uint8_t out[4]) {
    unsigned v = __builtin_bswap32((unsigned)packed);
    out[0] = v & 0xFF; out[1] = (v >> 8) & 0xFF;
    out[2] = (v >> 16) & 0xFF; out[3] = (v >> 24) & 0xFF;
}

// ---------------------------------------------------------------------------
// RB3UnpackMeshVerts — the per-vertex CPU unpack shared by DrawMesh and the L2
// GPU warm sweep (BandRnd::WarmGpuForDir). Reads owner->mVerts (uncompressed RB3
// Vert, Color32-packed) OR owner->mCompressedVerts (Xbox-compressed, Be*-decoded),
// filling the static OR skinned engine layout per `skinned`. Returns the unpacked
// vert count, or -1 if the mesh has no geometry. Factored out so the warm sweep's
// pre-upload is byte-identical to the draw-time upload (same VB bytes -> the first
// real draw is a guaranteed cache hit). This is the dominant -O0-wasm cost class
// (research/09: Be*/Half2Float/GpuVertexSkinned family) — charge it at the caller.
// ---------------------------------------------------------------------------
int RB3UnpackMeshVerts(RndMesh* owner, bool skinned,
                       std::vector<GpuVertex>& gpuVerts,
                       std::vector<GpuVertexSkinned>& gpuVertsSkinned) {
    RndMesh::VertVector& verts = owner->mVerts;
    int nv = verts.size();
    if (nv > 0) {
        if (skinned) {
            gpuVertsSkinned.resize(nv);
            for (int i = 0; i < nv; i++) {
                const RndMesh::Vert& v = verts[i];
                GpuVertexSkinned& g = gpuVertsSkinned[i];
                g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
                g.norm[0] = v.norm.x; g.norm[1] = v.norm.y; g.norm[2] = v.norm.z;
                g.color[0] = v.color.fr(); g.color[1] = v.color.fg();
                g.color[2] = v.color.fb(); g.color[3] = v.color.fa();
                g.uv[0] = v.uv.x; g.uv[1] = v.uv.y;
                g.boneWeights[0] = v.boneWeights.GetX(); g.boneWeights[1] = v.boneWeights.GetY();
                g.boneWeights[2] = v.boneWeights.GetZ(); g.boneWeights[3] = v.boneWeights.GetW();
                g.boneIndices[0] = (uint8_t)v.boneIndices[0]; g.boneIndices[1] = (uint8_t)v.boneIndices[1];
                g.boneIndices[2] = (uint8_t)v.boneIndices[2]; g.boneIndices[3] = (uint8_t)v.boneIndices[3];
                g.pad = 0.0f;
                g.tangent[0] = 1.0f; g.tangent[1] = 0; g.tangent[2] = 0; g.tangent[3] = 1.0f;
            }
        } else {
            gpuVerts.resize(nv);
            for (int i = 0; i < nv; i++) {
                const RndMesh::Vert& v = verts[i];
                GpuVertex& g = gpuVerts[i];
                g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
                g.norm[0] = v.norm.x; g.norm[1] = v.norm.y; g.norm[2] = v.norm.z;
                g.color[0] = v.color.fr(); g.color[1] = v.color.fg();
                g.color[2] = v.color.fb(); g.color[3] = v.color.fa();
                g.uv[0] = v.uv.x; g.uv[1] = v.uv.y;
                g.tangent[0] = 1.0f; g.tangent[1] = 0; g.tangent[2] = 0; g.tangent[3] = 1.0f;
            }
        }
    } else if (owner->mCompressedVerts && owner->mNumCompressedVerts > 0) {
        nv = (int)owner->mNumCompressedVerts;
        const XboxCVert* cv = (const XboxCVert*)owner->mCompressedVerts;
        if (skinned) {
            gpuVertsSkinned.resize(nv);
            for (int i = 0; i < nv; i++) {
                GpuVertexSkinned& g = gpuVertsSkinned[i];
                g.pos[0] = BeFloat(cv[i].pos[0]); g.pos[1] = BeFloat(cv[i].pos[1]); g.pos[2] = BeFloat(cv[i].pos[2]);
                BeColor(cv[i].color, g.color);
                BeUV(cv[i].uv, g.uv);
                BeDec4n(cv[i].norm, g.norm);
                BeUDec4n(cv[i].b0, g.boneWeights);   // BLENDWEIGHT (UDEC4N)
                BeUByte4(cv[i].b1, g.boneIndices);   // BLENDINDICES (UBYTE4)
                g.pad = 0.0f;
                float t3[3]; BeDec4n(cv[i].tan, t3);
                g.tangent[0] = t3[0]; g.tangent[1] = t3[1]; g.tangent[2] = t3[2]; g.tangent[3] = 1.0f;
            }
        } else {
            gpuVerts.resize(nv);
            for (int i = 0; i < nv; i++) {
                GpuVertex& g = gpuVerts[i];
                g.pos[0] = BeFloat(cv[i].pos[0]); g.pos[1] = BeFloat(cv[i].pos[1]); g.pos[2] = BeFloat(cv[i].pos[2]);
                BeColor(cv[i].color, g.color);
                BeUV(cv[i].uv, g.uv);
                BeDec4n(cv[i].norm, g.norm);
                float t3[3]; BeDec4n(cv[i].tan, t3);
                g.tangent[0] = t3[0]; g.tangent[1] = t3[1]; g.tangent[2] = t3[2]; g.tangent[3] = 1.0f;
            }
        }
    } else {
        return -1; // no geometry
    }
    return skinned ? (int)gpuVertsSkinned.size() : (int)gpuVerts.size();
}

void RndMesh::OnSync(int) {
    // Geometry changed — mark this mesh's cached GPU vertex/index buffers dirty so
    // the next DrawMesh re-uploads them. This is the dirty signal dynamic meshes
    // (RndText sub-meshes, ribbons, procedural geometry) fire via RndMesh::Sync
    // when their verts mutate; without it the per-mesh GPU cache (sMeshGpu) would
    // keep drawing stale geometry. Clearing `uploaded` keeps the existing
    // wgpu::Buffers (re-`WriteBuffer`'d in place on re-upload if sizes match, or
    // recreated if vert/face counts changed) — no leak.
    auto it = sMeshGpu.find(this);
    if (it != sMeshGpu.end()) it->second.uploaded = false;
    // Bump this mesh's geometry generation. When `this` is a GEOMETRY OWNER for one
    // or more drawn proxies (SetGeomOwner), those proxies are keyed by their OWN
    // pointer in sMeshGpu and never see the `uploaded=false` above. Their DrawMesh
    // compares their stamped fpOwnerGen against this live gen and re-uploads when it
    // lags — this is what makes an approaching sustain Tail's tube (owner verts move
    // every frame, section count saturated -> count fingerprint unchanged) render
    // before it is held. Self-owned meshes also bump it, harmlessly: their own entry
    // already invalidated via `uploaded=false`, and DrawMesh skips the owner-gen
    // check when owner == mesh, so the stamp simply tracks along.
    ++sGeomSyncGen[this];
}

// ---------------------------------------------------------------------------
// RB3EnsureMeshGpu — idempotent mesh-upload helper extracted from DrawMesh's
// needUpload block. Unpacks (shared RB3UnpackMeshVerts) + uploads VB/IB + stamps
// the sMeshGpu fingerprint with the SAME keys DrawMesh uses, AND populates the L1
// skinned bind-vert cache. So after a warm pass, the first real draw of this mesh
// sees needUpload==false (geometry-buffer reuse), skipUnpack==true (no re-unpack),
// and the skinned shard guard reads the warmed cache — zero reveal-frame work.
// Returns true iff it actually uploaded (cache miss). No render-pass dependency:
// it only creates+writes buffers (queue ops), so it is safe to call outside an
// open pass during the loading dwell. Used by BandRnd::WarmGpuForDir.
// ---------------------------------------------------------------------------
bool RB3EnsureMeshGpu(BandRnd& rnd, RndMesh* mesh) {
    if (!mesh) return false;
    RndMesh* owner = mesh->GeomOwner();
    if (!owner) owner = mesh;
    std::vector<RndMesh::Face>& faces = owner->mFaces;
    int nf = (int)faces.size();
    if (nf <= 0) return false;

    bool skinned = owner->IsSkinned();
    int nvSrc = owner->mVerts.size();
    int fpVertsKey = (nvSrc > 0) ? nvSrc : (int)owner->mNumCompressedVerts;

    RB3MeshEntry& meshEntry = sMeshGpu[mesh];
    // Mirror DrawMesh's owner-generation check so a warmed proxy whose owner verts
    // changed (same count) is treated as a cache miss here too — and, critically, so
    // the warm pass stamps fpOwnerGen identically, or the first real draw would see
    // a stale stamp and re-upload once (harmless but defeats the warm win).
    bool ownerGenStale = (owner != mesh) &&
                         (meshEntry.fpOwnerGen != LookupGeomSyncGen(owner));
    bool needUpload = !meshEntry.uploaded ||
                      meshEntry.ownerKey != (const void*)owner ||
                      meshEntry.fpVerts != fpVertsKey || meshEntry.fpFaces != nf ||
                      meshEntry.fpSkinned != skinned || ownerGenStale;
    // Already resident with the warm L1 caches in place -> nothing to do. (For
    // skinned meshes, only consider it warm once the bind-vert cache is populated,
    // so the warmed first-draw shard guard has data.)
    if (!needUpload && (!skinned || !meshEntry.cachedSkinnedVerts.empty()))
        return false;

    std::vector<GpuVertexRB3> gpuVerts;
    std::vector<GpuVertexSkinned> gpuVertsSkinned;
    int nv = RB3UnpackMeshVerts(owner, skinned, gpuVerts, gpuVertsSkinned);
    if (nv < 0) return false;

    // L1: warm the skinned bind-vert cache (read every frame by the shard guard).
    if (skinned)
        meshEntry.cachedSkinnedVerts = gpuVertsSkinned;

    // If the GPU buffers are already current (only the skinned cache was missing),
    // refresh the cache above and stop — don't recreate identical buffers.
    if (!needUpload)
        return false;

    // Local bounding sphere for static meshes (mirrors DrawMesh's needUpload arm —
    // compressed venue meshes have no other place to recompute it).
    if (!skinned && nv > 0) {
        float mn3[3] = { 1e30f, 1e30f, 1e30f }, mx3[3] = { -1e30f, -1e30f, -1e30f };
        for (int i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) {
                float p = gpuVerts[i].pos[k];
                if (p < mn3[k]) mn3[k] = p;
                if (p > mx3[k]) mx3[k] = p;
            }
        Vector3 center((mn3[0] + mx3[0]) * 0.5f, (mn3[1] + mx3[1]) * 0.5f,
                       (mn3[2] + mx3[2]) * 0.5f);
        float dx = mx3[0] - mn3[0], dy = mx3[1] - mn3[1], dz = mx3[2] - mn3[2];
        float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        Sphere localSphere;
        localSphere.Set(center, radius);
        mesh->SetSphere(localSphere);
    }

    std::vector<uint16_t> indices;
    indices.reserve(nf * 3);
    for (int i = 0; i < nf; i++) {
        indices.push_back(faces[i].v1);
        indices.push_back(faces[i].v2);
        indices.push_back(faces[i].v3);
    }
    {
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshVB";
        bd.size = skinned ? ((uint64_t)nv * sizeof(GpuVertexSkinned))
                          : ((uint64_t)nv * sizeof(GpuVertexRB3));
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        meshEntry.vbuf = rnd.mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        rnd.mGpu.Queue().WriteBuffer(meshEntry.vbuf, 0,
                                     skinned ? (const void*)gpuVertsSkinned.data()
                                             : (const void*)gpuVerts.data(),
                                     bd.size);
    }
    {
        uint64_t isz = indices.size() * sizeof(uint16_t);
        uint64_t padded = (isz + 3) & ~3ull;
        indices.resize(padded / sizeof(uint16_t), 0);
        wgpu::BufferDescriptor bd{};
        bd.label = "MeshIB"; bd.size = padded;
        bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        meshEntry.ibuf = rnd.mGpu.Device().CreateBuffer(&bd);
        sMeshBufCreatesThisFrame++;
        rnd.mGpu.Queue().WriteBuffer(meshEntry.ibuf, 0, indices.data(), padded);
    }
    meshEntry.indexCount = (uint32_t)(nf * 3);
    meshEntry.skinned    = skinned;
    meshEntry.ownerKey   = (const void*)owner;
    meshEntry.fpVerts    = fpVertsKey;
    meshEntry.fpFaces    = nf;
    meshEntry.fpSkinned  = skinned;
    meshEntry.fpOwnerGen = LookupGeomSyncGen(owner);
    meshEntry.uploaded   = true;
    return true;
}
