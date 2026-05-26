#pragma once

#include <webgpu/webgpu_cpp.h>
#include <cstdint>

class RndMesh;

// GPU vertex layout after unpacking from RndMesh::Vert
struct GpuVertex {
    float pos[3];       // 0  - position
    float norm[3];      // 12 - normal
    float color[4];     // 24 - vertex color (RGBA float)
    float uv[2];        // 40 - texture coordinates
    float tangent[4];   // 48 - tangent (xyz) + bitangent sign (w)
};
static_assert(sizeof(GpuVertex) == 64, "GpuVertex must be 64 bytes");

// Skinned vertex adds bone data
struct GpuVertexSkinned {
    float pos[3];       // 0
    float norm[3];      // 12
    float color[4];     // 24
    float uv[2];        // 40
    float boneWeights[4]; // 48
    uint8_t boneIndices[4]; // 64
    float pad;          // 68 - alignment padding
    float tangent[4];   // 72 - tangent (xyz) + bitangent sign (w)
};
static_assert(sizeof(GpuVertexSkinned) == 88, "GpuVertexSkinned must be 88 bytes");

namespace VertexFormats {

// WebGPU vertex buffer layouts (static singletons)
const wgpu::VertexBufferLayout& StaticLayout();
const wgpu::VertexBufferLayout& SkinnedLayout();

// Unpack RndMesh vertices into GPU format
// Returns number of vertices written
int UnpackStaticVertices(const RndMesh& mesh, GpuVertex* out, int maxVerts);
int UnpackSkinnedVertices(const RndMesh& mesh, GpuVertexSkinned* out, int maxVerts);

// Unpack Xbox 360 compressed vertices into GPU format
int UnpackCompressedVertices(const unsigned char* compressedData, int numVerts,
                             GpuVertex* out, int maxVerts);

// Unpack Xbox 360 compressed vertices with bone data into skinned GPU format
int UnpackCompressedSkinnedVertices(const unsigned char* compressedData, int numVerts,
                                     GpuVertexSkinned* out, int maxVerts);

} // namespace VertexFormats
