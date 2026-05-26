#include "gfx/VertexFormats.h"
#include "rndobj/Mesh.h"
#include "rndobj/MeshVertCompress.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace VertexFormats {

// ============================================================================
// Helper to build VertexAttribute (Dawn structs have nextInChain as first member)
// ============================================================================

static wgpu::VertexAttribute MakeAttr(wgpu::VertexFormat fmt, uint64_t offset, uint32_t loc) {
    wgpu::VertexAttribute a{};
    a.format = fmt;
    a.offset = offset;
    a.shaderLocation = loc;
    return a;
}

// ============================================================================
// Static vertex layout: pos(3f) + norm(3f) + color(4f) + uv(2f) = 48 bytes
// ============================================================================

static wgpu::VertexAttribute sStaticAttrs[5];
static wgpu::VertexBufferLayout sStaticLayout;
static bool sStaticInited = false;

static void InitStaticLayout() {
    if (sStaticInited) return;
    sStaticAttrs[0] = MakeAttr(wgpu::VertexFormat::Float32x3, 0,  0); // position
    sStaticAttrs[1] = MakeAttr(wgpu::VertexFormat::Float32x3, 12, 1); // normal
    sStaticAttrs[2] = MakeAttr(wgpu::VertexFormat::Float32x4, 24, 2); // color
    sStaticAttrs[3] = MakeAttr(wgpu::VertexFormat::Float32x2, 40, 3); // uv
    sStaticAttrs[4] = MakeAttr(wgpu::VertexFormat::Float32x4, 48, 4); // tangent

    sStaticLayout.arrayStride = sizeof(GpuVertex);
    sStaticLayout.stepMode = wgpu::VertexStepMode::Vertex;
    sStaticLayout.attributeCount = 5;
    sStaticLayout.attributes = sStaticAttrs;
    sStaticInited = true;
}

const wgpu::VertexBufferLayout& StaticLayout() {
    InitStaticLayout();
    return sStaticLayout;
}

// ============================================================================
// Skinned vertex layout: static + boneWeights(4f) + boneIndices(4u8) = 72 bytes
// ============================================================================

static wgpu::VertexAttribute sSkinnedAttrs[7];
static wgpu::VertexBufferLayout sSkinnedLayout;
static bool sSkinnedInited = false;

static void InitSkinnedLayout() {
    if (sSkinnedInited) return;
    sSkinnedAttrs[0] = MakeAttr(wgpu::VertexFormat::Float32x3, 0,  0); // position
    sSkinnedAttrs[1] = MakeAttr(wgpu::VertexFormat::Float32x3, 12, 1); // normal
    sSkinnedAttrs[2] = MakeAttr(wgpu::VertexFormat::Float32x4, 24, 2); // color
    sSkinnedAttrs[3] = MakeAttr(wgpu::VertexFormat::Float32x2, 40, 3); // uv
    sSkinnedAttrs[4] = MakeAttr(wgpu::VertexFormat::Float32x4, 48, 4); // boneWeights
    sSkinnedAttrs[5] = MakeAttr(wgpu::VertexFormat::Uint8x4,   64, 5); // boneIndices
    sSkinnedAttrs[6] = MakeAttr(wgpu::VertexFormat::Float32x4, 72, 6); // tangent

    sSkinnedLayout.arrayStride = sizeof(GpuVertexSkinned);
    sSkinnedLayout.stepMode = wgpu::VertexStepMode::Vertex;
    sSkinnedLayout.attributeCount = 7;
    sSkinnedLayout.attributes = sSkinnedAttrs;
    sSkinnedInited = true;
}

const wgpu::VertexBufferLayout& SkinnedLayout() {
    InitSkinnedLayout();
    return sSkinnedLayout;
}

// ============================================================================
// Vertex unpacking from RndMesh::Vert
// ============================================================================

int UnpackStaticVertices(const RndMesh& mesh, GpuVertex* out, int maxVerts) {
    // Access verts through mGeomOwner (same as mesh.Verts())
    RndMesh* owner = const_cast<RndMesh*>(&mesh);
    int numVerts = std::min(owner->NumVerts(), maxVerts);

    for (int i = 0; i < numVerts; i++) {
        const RndMesh::Vert& v = owner->Verts(i);
        GpuVertex& gv = out[i];

        gv.pos[0] = v.pos.x;
        gv.pos[1] = v.pos.y;
        gv.pos[2] = v.pos.z;

        gv.norm[0] = v.norm.x;
        gv.norm[1] = v.norm.y;
        gv.norm[2] = v.norm.z;

        gv.color[0] = v.color.red;
        gv.color[1] = v.color.green;
        gv.color[2] = v.color.blue;
        gv.color[3] = v.color.alpha;

        gv.uv[0] = v.tex.x;
        gv.uv[1] = v.tex.y;

        // Tangent will be computed by MikkTSpace after unpacking
        gv.tangent[0] = 1.0f; gv.tangent[1] = 0.0f;
        gv.tangent[2] = 0.0f; gv.tangent[3] = 1.0f;
    }
    return numVerts;
}

int UnpackSkinnedVertices(const RndMesh& mesh, GpuVertexSkinned* out, int maxVerts) {
    RndMesh* owner = const_cast<RndMesh*>(&mesh);
    int numVerts = std::min(owner->NumVerts(), maxVerts);

    for (int i = 0; i < numVerts; i++) {
        const RndMesh::Vert& v = owner->Verts(i);
        GpuVertexSkinned& gv = out[i];

        gv.pos[0] = v.pos.x;
        gv.pos[1] = v.pos.y;
        gv.pos[2] = v.pos.z;

        gv.norm[0] = v.norm.x;
        gv.norm[1] = v.norm.y;
        gv.norm[2] = v.norm.z;

        gv.color[0] = v.color.red;
        gv.color[1] = v.color.green;
        gv.color[2] = v.color.blue;
        gv.color[3] = v.color.alpha;

        gv.uv[0] = v.tex.x;
        gv.uv[1] = v.tex.y;

        gv.boneWeights[0] = v.boneWeights.x;
        gv.boneWeights[1] = v.boneWeights.y;
        gv.boneWeights[2] = v.boneWeights.z;
        gv.boneWeights[3] = v.boneWeights.w;

        gv.boneIndices[0] = (uint8_t)v.boneIndices[0];
        gv.boneIndices[1] = (uint8_t)v.boneIndices[1];
        gv.boneIndices[2] = (uint8_t)v.boneIndices[2];
        gv.boneIndices[3] = (uint8_t)v.boneIndices[3];

        gv.pad = 0.0f;

        // Tangent will be computed by MikkTSpace after unpacking
        gv.tangent[0] = 1.0f; gv.tangent[1] = 0.0f;
        gv.tangent[2] = 0.0f; gv.tangent[3] = 1.0f;
    }
    return numVerts;
}

// ============================================================================
// Unpack Xbox 360 compressed vertices to GpuVertex
// CompressedVertex_Xbox: 36 bytes each, big-endian on disc
//
// IMPORTANT: The struct field names are MISLEADING. The actual D3D vertex
// declaration layout (from rnddx9/Mesh.cpp) is:
//   mPosX/Y/Z  = FLOAT3     POSITION      (3 floats, 12 bytes)
//   mColor     = D3DCOLOR   COLOR          (packed ARGB, 4 bytes)
//   mNormal    = FLOAT16_2  TEXCOORD       (UV as two half-floats!)
//   mTangent   = DEC4N      NORMAL         (normal as 10-10-10-2)
//   mBinormal  = DEC4N      TANGENT        (tangent as 10-10-10-2)
//   mBoneIndices = UDEC4N   BLENDWEIGHT    (bone weights)
//   mBoneWeights = UBYTE4   BLENDINDICES   (bone indices)
// ============================================================================

static float UnpackFloat_BE(int bits) {
    // Byte-swap int from big-endian to little-endian, then reinterpret as float
    unsigned int val = __builtin_bswap32((unsigned int)bits);
    float f;
    memcpy(&f, &val, 4);
    return f;
}

static void UnpackColor_BE(int packed, float out[4]) {
    // Byte-swap first
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    // ABGR packed: R=low byte, A=high byte
    out[0] = (float)((val >> 0) & 0xFF) / 255.0f;  // R
    out[1] = (float)((val >> 8) & 0xFF) / 255.0f;  // G
    out[2] = (float)((val >> 16) & 0xFF) / 255.0f; // B
    out[3] = (float)((val >> 24) & 0xFF) / 255.0f; // A
}

static void UnpackDEC4N_BE(int packed, float out[3]) {
    // DEC4N: 10-10-10-2 signed normalized, big-endian
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    // x=bits[0:9], y=bits[10:19], z=bits[20:29], w=bits[30:31]
    int ix = (int)(val << 22) >> 22;  // sign-extend 10 bits
    int iy = (int)(val << 12) >> 22;
    int iz = (int)(val << 2) >> 22;
    out[0] = ix / 511.0f;
    out[1] = iy / 511.0f;
    out[2] = iz / 511.0f;
}

static float HalfToFloat(unsigned short h) {
    // IEEE 754 half-precision: 1 sign, 5 exponent, 10 mantissa
    unsigned int sign = (h >> 15) & 1;
    unsigned int exp  = (h >> 10) & 0x1F;
    unsigned int mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) {
            // Zero
            unsigned int f = sign << 31;
            float result;
            memcpy(&result, &f, 4);
            return result;
        }
        // Denormalized: convert to normalized float
        float val = (float)mant / 1024.0f;
        val *= (1.0f / 16384.0f); // 2^-14
        return sign ? -val : val;
    } else if (exp == 0x1F) {
        // Inf/NaN
        unsigned int f = (sign << 31) | 0x7F800000 | (mant << 13);
        float result;
        memcpy(&result, &f, 4);
        return result;
    }

    // Normalized: rebias exponent from half (bias=15) to float (bias=127)
    unsigned int f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float result;
    memcpy(&result, &f, 4);
    return result;
}

static void UnpackFloat16x2_BE(int packed, float out[2]) {
    // Byte-swap first, then extract two 16-bit half-floats
    // Packing: (halfU << 16) | halfV (before endian swap)
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    unsigned short halfU = (val >> 16) & 0xFFFF;  // tex.x in upper 16 bits
    unsigned short halfV = val & 0xFFFF;           // tex.y in lower 16 bits
    out[0] = HalfToFloat(halfU);
    out[1] = HalfToFloat(halfV);
}

// Extract the 2-bit w from DEC4N as a bitangent sign (±1.0)
static float UnpackDEC4N_Sign_BE(int packed) {
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    int iw = (int)(val) >> 30;  // sign-extend 2-bit w
    return (iw >= 0) ? 1.0f : -1.0f;
}

int UnpackCompressedVertices(const unsigned char* compressedData, int numVerts,
                             GpuVertex* out, int maxVerts) {
    int count = std::min(numVerts, maxVerts);
    const CompressedVertex_Xbox* cverts = (const CompressedVertex_Xbox*)compressedData;

    for (int i = 0; i < count; i++) {
        const CompressedVertex_Xbox& cv = cverts[i];
        GpuVertex& gv = out[i];

        // Position: float stored as int bits (big-endian)
        gv.pos[0] = UnpackFloat_BE(cv.mPosX);
        gv.pos[1] = UnpackFloat_BE(cv.mPosY);
        gv.pos[2] = UnpackFloat_BE(cv.mPosZ);

        // Color: packed RGBA (D3DCOLOR at offset 12)
        UnpackColor_BE(cv.mColor, gv.color);

        // UV: FLOAT16_2 stored in mNormal field (D3D TEXCOORD at offset 16)
        UnpackFloat16x2_BE(cv.mNormal, gv.uv);

        // Normal: DEC4N stored in mTangent field (D3D NORMAL at offset 20)
        UnpackDEC4N_BE(cv.mTangent, gv.norm);

        // Tangent: DEC4N stored in mBinormal field (D3D TANGENT at offset 24)
        // The 2-bit w component encodes the bitangent sign (handedness)
        float tangent3[3];
        UnpackDEC4N_BE(cv.mBinormal, tangent3);
        gv.tangent[0] = tangent3[0];
        gv.tangent[1] = tangent3[1];
        gv.tangent[2] = tangent3[2];
        gv.tangent[3] = UnpackDEC4N_Sign_BE(cv.mBinormal);
    }
    return count;
}

// ============================================================================
// Unpack Xbox 360 compressed vertices with bone weights/indices
// Note: CompressedVertex_Xbox field names are SWAPPED for bone data:
//   mBoneIndices = UDEC4N  BLENDWEIGHT  (bone weights as 10-10-10-2 unsigned normalized)
//   mBoneWeights = UBYTE4  BLENDINDICES (bone indices as 4 bytes)
// ============================================================================

static void UnpackUDEC4N_BE(int packed, float out[4]) {
    // UDEC4N: 10-10-10-2 unsigned normalized, big-endian
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    unsigned int ix = val & 0x3FF;
    unsigned int iy = (val >> 10) & 0x3FF;
    unsigned int iz = (val >> 20) & 0x3FF;
    unsigned int iw = (val >> 30) & 0x3;
    out[0] = ix / 1023.0f;
    out[1] = iy / 1023.0f;
    out[2] = iz / 1023.0f;
    out[3] = iw / 3.0f;
}

static void UnpackUBYTE4_BE(int packed, uint8_t out[4]) {
    // UBYTE4: 4 individual bytes packed into a 32-bit int.
    // The int is stored big-endian in the file, so on a LE system the byte
    // order within the native int is reversed. We must bswap to restore the
    // original Xbox byte order before extracting individual indices.
    // Packing on Xbox: value = idx0 + idx1*256 + idx2*65536 + idx3*16M
    // File bytes (BE): [idx3, idx2, idx1, idx0]
    // LE native int:   idx3 + idx2*256 + idx1*65536 + idx0*16M (reversed)
    // After bswap:     idx0 + idx1*256 + idx2*65536 + idx3*16M (correct)
    unsigned int val = __builtin_bswap32((unsigned int)packed);
    out[0] = (val) & 0xFF;
    out[1] = (val >> 8) & 0xFF;
    out[2] = (val >> 16) & 0xFF;
    out[3] = (val >> 24) & 0xFF;
}

int UnpackCompressedSkinnedVertices(const unsigned char* compressedData, int numVerts,
                                     GpuVertexSkinned* out, int maxVerts) {
    int count = std::min(numVerts, maxVerts);
    const CompressedVertex_Xbox* cverts = (const CompressedVertex_Xbox*)compressedData;

    for (int i = 0; i < count; i++) {
        const CompressedVertex_Xbox& cv = cverts[i];
        GpuVertexSkinned& gv = out[i];

        // Position
        gv.pos[0] = UnpackFloat_BE(cv.mPosX);
        gv.pos[1] = UnpackFloat_BE(cv.mPosY);
        gv.pos[2] = UnpackFloat_BE(cv.mPosZ);

        // Color
        UnpackColor_BE(cv.mColor, gv.color);

        // UV: FLOAT16_2 stored in mNormal field
        UnpackFloat16x2_BE(cv.mNormal, gv.uv);

        // Normal: DEC4N stored in mTangent field
        UnpackDEC4N_BE(cv.mTangent, gv.norm);

        // Bone weights: UDEC4N stored in mBoneIndices field (names swapped!)
        UnpackUDEC4N_BE(cv.mBoneIndices, gv.boneWeights);

        // Bone indices: UBYTE4 stored in mBoneWeights field (names swapped!)
        UnpackUBYTE4_BE(cv.mBoneWeights, gv.boneIndices);

        gv.pad = 0.0f;

        // Tangent: DEC4N stored in mBinormal field (D3D TANGENT at offset 24)
        float tangent3[3];
        UnpackDEC4N_BE(cv.mBinormal, tangent3);
        gv.tangent[0] = tangent3[0];
        gv.tangent[1] = tangent3[1];
        gv.tangent[2] = tangent3[2];
        gv.tangent[3] = UnpackDEC4N_Sign_BE(cv.mBinormal);
    }
    return count;
}

} // namespace VertexFormats
