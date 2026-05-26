// DC3 Native Port — GPU mesh resource cache
// Manages GPU vertex/index buffers for RndMesh objects.
// Contains: upload logic, MikkTSpace tangent generation, frame stats, side table.

#include "platform/MeshGpuCache.h"
#include "platform/Rnd_Wgpu.h"
#include "gfx/VertexFormats.h"
#include "rndobj/Mesh.h"

#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "gfx/mikktspace.h"
}

// ============================================================================
// GPU mesh data side table
// ============================================================================

static std::unordered_map<RndMesh*, GpuMeshData> sMeshGpuData;

// ============================================================================
// Frame statistics
// ============================================================================

static int sDrawCallsThisFrame = 0;
static int sFrameCounter = 0;

void RndMesh_ResetFrameStats() {
    sDrawCallsThisFrame = 0;
    sFrameCounter++;
}

void IncrementMeshDrawCalls() {
    sDrawCallsThisFrame++;
}

int GetMeshFrameCounter() {
    return sFrameCounter;
}

int GetMeshDrawCallsThisFrame() {
    return sDrawCallsThisFrame;
}

// ============================================================================
// Side table helpers
// ============================================================================

void CleanupGpuMesh(RndMesh* mesh) {
    sMeshGpuData.erase(mesh);
}

void SetMeshDebugLabel(RndMesh* mesh, const char* label) {
    sMeshGpuData[mesh].debugLabel = label;
}

void SetMeshDepthBias(RndMesh* mesh, int32_t bias) {
    sMeshGpuData[mesh].depthBias = bias;
}

const char* MeshLabel(RndMesh* mesh) {
    auto it = sMeshGpuData.find(mesh);
    if (it != sMeshGpuData.end() && !it->second.debugLabel.empty())
        return it->second.debugLabel.c_str();
    return mesh->Name();
}

GpuMeshData* GetMeshGpuData(RndMesh* mesh) {
    auto it = sMeshGpuData.find(mesh);
    if (it != sMeshGpuData.end())
        return &it->second;
    return nullptr;
}

// Invalidate GPU cache when mesh data changes (called from RndMesh::Sync)
void RndMesh::OnSync(int flags) {
    auto it = sMeshGpuData.find(this);
    if (it != sMeshGpuData.end()) {
        it->second.uploaded = false;
    }
}

// ============================================================================
// Fix zero-alpha vertex colors (common for texture-only meshes)
// ============================================================================

template<typename VertType>
static void FixZeroAlpha(VertType* verts, int count) {
    bool allAlphaZero = true;
    bool allRGBZero = true;
    int checkCount = count < 10 ? count : 10;
    for (int i = 0; i < checkCount; i++) {
        if (verts[i].color[3] > 0.001f) { allAlphaZero = false; }
        if (verts[i].color[0] > 0.001f || verts[i].color[1] > 0.001f || verts[i].color[2] > 0.001f) {
            allRGBZero = false;
        }
    }
    if (allAlphaZero) {
        for (int i = 0; i < count; i++) {
            verts[i].color[3] = 1.0f;
            // Only force RGB to white if it's also all zero (truly unused vertex colors).
            // Preserve meaningful RGB (e.g. baked AO) when only alpha is missing.
            if (allRGBZero) {
                verts[i].color[0] = verts[i].color[1] = verts[i].color[2] = 1.0f;
            }
        }
    }
}

// ============================================================================
// MikkTSpace tangent generation callbacks
// ============================================================================

struct MikkUserData {
    void* verts;         // GpuVertex* or GpuVertexSkinned*
    const uint16_t* indices;
    int numFaces;
    int numVerts;
    bool skinned;
};

template<typename V>
static V& GetMikkVert(MikkUserData* ud, int face, int vert) {
    int idx = ((const uint16_t*)ud->indices)[face * 3 + vert];
    return ((V*)ud->verts)[idx];
}

static int mikkGetNumFaces(const SMikkTSpaceContext* ctx) {
    return ((MikkUserData*)ctx->m_pUserData)->numFaces;
}
static int mikkGetNumVerticesOfFace(const SMikkTSpaceContext*, int) { return 3; }

static void mikkGetPosition(const SMikkTSpaceContext* ctx, float pos[], int face, int vert) {
    auto* ud = (MikkUserData*)ctx->m_pUserData;
    if (ud->skinned) {
        auto& v = GetMikkVert<GpuVertexSkinned>(ud, face, vert);
        pos[0] = v.pos[0]; pos[1] = v.pos[1]; pos[2] = v.pos[2];
    } else {
        auto& v = GetMikkVert<GpuVertex>(ud, face, vert);
        pos[0] = v.pos[0]; pos[1] = v.pos[1]; pos[2] = v.pos[2];
    }
}
static void mikkGetNormal(const SMikkTSpaceContext* ctx, float norm[], int face, int vert) {
    auto* ud = (MikkUserData*)ctx->m_pUserData;
    if (ud->skinned) {
        auto& v = GetMikkVert<GpuVertexSkinned>(ud, face, vert);
        norm[0] = v.norm[0]; norm[1] = v.norm[1]; norm[2] = v.norm[2];
    } else {
        auto& v = GetMikkVert<GpuVertex>(ud, face, vert);
        norm[0] = v.norm[0]; norm[1] = v.norm[1]; norm[2] = v.norm[2];
    }
}
static void mikkGetTexCoord(const SMikkTSpaceContext* ctx, float uv[], int face, int vert) {
    auto* ud = (MikkUserData*)ctx->m_pUserData;
    if (ud->skinned) {
        auto& v = GetMikkVert<GpuVertexSkinned>(ud, face, vert);
        uv[0] = v.uv[0]; uv[1] = v.uv[1];
    } else {
        auto& v = GetMikkVert<GpuVertex>(ud, face, vert);
        uv[0] = v.uv[0]; uv[1] = v.uv[1];
    }
}
static void mikkSetTSpaceBasic(const SMikkTSpaceContext* ctx,
    const float tangent[], float sign, int face, int vert) {
    auto* ud = (MikkUserData*)ctx->m_pUserData;
    if (ud->skinned) {
        auto& v = GetMikkVert<GpuVertexSkinned>(ud, face, vert);
        v.tangent[0] = tangent[0]; v.tangent[1] = tangent[1];
        v.tangent[2] = tangent[2]; v.tangent[3] = sign;
    } else {
        auto& v = GetMikkVert<GpuVertex>(ud, face, vert);
        v.tangent[0] = tangent[0]; v.tangent[1] = tangent[1];
        v.tangent[2] = tangent[2]; v.tangent[3] = sign;
    }
}

static void ComputeMikkTangents(void* verts, const uint16_t* indices,
    int numFaces, int numVerts, bool skinned) {
    MikkUserData ud;
    ud.verts = verts;
    ud.indices = indices;
    ud.numFaces = numFaces;
    ud.numVerts = numVerts;
    ud.skinned = skinned;

    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = mikkGetNumFaces;
    iface.m_getNumVerticesOfFace = mikkGetNumVerticesOfFace;
    iface.m_getPosition = mikkGetPosition;
    iface.m_getNormal = mikkGetNormal;
    iface.m_getTexCoord = mikkGetTexCoord;
    iface.m_setTSpaceBasic = mikkSetTSpaceBasic;

    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &ud;

    genTangSpaceDefault(&ctx);
}

// ============================================================================
// Helper: Upload mesh vertex/index data to GPU
// ============================================================================

bool EnsureMeshUploaded(RndMesh* mesh) {
    if (!gWgpuRnd) return false;

    bool isTextMesh = !mesh->Name()[0];
    auto it = sMeshGpuData.find(mesh);
    if (it != sMeshGpuData.end() && it->second.uploaded && !isTextMesh) {
        return true;
    }

    RndMesh* geomOwner = mesh->GetGeomOwner();
    if (!geomOwner) geomOwner = mesh;

    int numVerts = geomOwner->NumVerts();
    int numFaces = geomOwner->NumFaces();
    int numCompressedVerts = geomOwner->NumCompressedVerts();
    bool skinned = mesh->IsSkinned();

    // Check if we have vertices (either uncompressed or compressed)
    if (numVerts <= 0 && numCompressedVerts <= 0) {
        static int sNoVertLog = 0;
        if (sNoVertLog++ < 5) fprintf(stderr, "Mesh_Wgpu: skipping '%s' — no vertices (owner='%s' ownerVerts=%d)\n",
            mesh->Name(), geomOwner->Name(), geomOwner->NumVerts());
        return false;
    }
    if (numFaces <= 0) {
        static int sNoFaceLog = 0;
        if (sNoFaceLog++ < 5) fprintf(stderr, "Mesh_Wgpu: skipping '%s' — no faces\n", mesh->Name());
        return false;
    }

    int vertCount = (numVerts > 0) ? numVerts : numCompressedVerts;
    bool isCompressed = (numCompressedVerts > 0 && geomOwner->CompressedVerts());

    // Skip MikkTSpace tangent generation on re-uploads (mesh was previously uploaded
    // but invalidated by Sync). Dynamic meshes like HamRibbon re-sync every frame;
    // recomputing MikkTSpace tangents each time is O(n²) in MergeVertsFast and causes
    // multi-second hangs. Tangents only matter for normal-mapped materials, which
    // dynamic meshes don't use.
    bool isReupload = (it != sMeshGpuData.end());

    // Build index buffer first (needed for MikkTSpace tangent generation)
    int numIndices = numFaces * 3;
    int allocIndices = (numIndices + 1) & ~1; // round up to even for 4-byte alignment
    uint16_t* indices = new uint16_t[allocIndices]();
    auto& faces = geomOwner->Faces();
    for (int i = 0; i < numFaces; i++) {
        indices[i * 3 + 0] = faces[i].v1;
        indices[i * 3 + 1] = faces[i].v2;
        indices[i * 3 + 2] = faces[i].v3;
    }

    wgpu::Buffer vertexBuf;
    int unpacked = 0;

    if (skinned) {
        // Skinned vertex path
        GpuVertexSkinned* verts = new GpuVertexSkinned[vertCount];
        if (isCompressed) {
            unpacked = VertexFormats::UnpackCompressedSkinnedVertices(
                geomOwner->CompressedVerts(), numCompressedVerts, verts, vertCount);
        } else {
            unpacked = VertexFormats::UnpackSkinnedVertices(*geomOwner, verts, vertCount);
            // Compute tangents via MikkTSpace for uncompressed meshes on first upload only.
            // (compressed meshes already have tangent data from the original Xbox vertex stream)
            if (unpacked > 0 && !isReupload) {
                ComputeMikkTangents(verts, indices, numFaces, unpacked, true);
            }
        }
        if (unpacked <= 0) {
            fprintf(stderr, "Mesh_Wgpu: failed to unpack skinned vertices for '%s'\n", mesh->Name());
            delete[] verts;
            delete[] indices;
            return false;
        }

        // (vertex color fix follows)

        // Fix zero vertex colors — many meshes don't use vertex color and have all-zero
        // RGBA, which would multiply baseColor to black in the shader. FixZeroAlpha is
        // conservative: only modifies if ALL sampled vertices have zero alpha.
        FixZeroAlpha(verts, unpacked);

        wgpu::BufferDescriptor vbDesc{};
        vbDesc.label = MeshLabel(mesh);
        vbDesc.size = unpacked * sizeof(GpuVertexSkinned);
        vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertexBuf = gWgpuRnd->Gpu().Device().CreateBuffer(&vbDesc);
        gWgpuRnd->Gpu().Queue().WriteBuffer(vertexBuf, 0, verts, unpacked * sizeof(GpuVertexSkinned));
        delete[] verts;
    } else {
        // Static vertex path
        GpuVertex* verts = new GpuVertex[vertCount];
        if (isCompressed) {
            unpacked = VertexFormats::UnpackCompressedVertices(
                geomOwner->CompressedVerts(), numCompressedVerts, verts, vertCount);
        } else {
            unpacked = VertexFormats::UnpackStaticVertices(*geomOwner, verts, vertCount);
            // Compute tangents via MikkTSpace for uncompressed meshes on first upload only.
            if (unpacked > 0 && !isReupload) {
                ComputeMikkTangents(verts, indices, numFaces, unpacked, false);
            }
        }
        if (unpacked <= 0) {
            fprintf(stderr, "Mesh_Wgpu: failed to unpack vertices for '%s' (verts=%d, compressed=%d)\n",
                    mesh->Name(), numVerts, numCompressedVerts);
            delete[] verts;
            delete[] indices;
            return false;
        }

        // Fix zero vertex colors — many meshes don't use vertex color and have all-zero
        // RGBA, which would multiply baseColor to black in the shader. FixZeroAlpha is
        // conservative: only modifies if ALL sampled vertices have zero alpha.
        FixZeroAlpha(verts, unpacked);

        wgpu::BufferDescriptor vbDesc{};
        vbDesc.label = MeshLabel(mesh);
        vbDesc.size = unpacked * sizeof(GpuVertex);
        vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertexBuf = gWgpuRnd->Gpu().Device().CreateBuffer(&vbDesc);
        gWgpuRnd->Gpu().Queue().WriteBuffer(vertexBuf, 0, verts, unpacked * sizeof(GpuVertex));
        delete[] verts;
    }

    size_t ibAlignedSize = (numIndices * sizeof(uint16_t) + 3) & ~3u;

    wgpu::BufferDescriptor ibDesc{};
    ibDesc.label = MeshLabel(mesh);
    ibDesc.size = ibAlignedSize;
    ibDesc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer indexBuf = gWgpuRnd->Gpu().Device().CreateBuffer(&ibDesc);
    gWgpuRnd->Gpu().Queue().WriteBuffer(indexBuf, 0, indices, ibAlignedSize);
    delete[] indices;

    GpuMeshData& data = sMeshGpuData[mesh];
    data.vertexBuffer = vertexBuf;
    data.indexBuffer = indexBuf;
    data.numIndices = numIndices;
    data.numVertices = unpacked;
    data.skinned = skinned;
    data.uploaded = true;
    return true;
}
