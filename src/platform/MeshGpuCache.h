// DC3 Native Port — GPU mesh resource cache
// Manages GPU vertex/index buffers for RndMesh objects via a side table.
// Separated from Mesh_Wgpu.cpp (draw logic) for modularity.

#pragma once

#include <cstdint>
#include <string>
#include <webgpu/webgpu_cpp.h>

class RndMesh;

// Per-mesh GPU resource data (vertex buffer, index buffer, metadata)
struct GpuMeshData {
    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    int numIndices = 0;
    int numVertices = 0;
    bool skinned = false;
    bool uploaded = false;
    int32_t depthBias = 0;  // set by viewer for combined meshes
    std::string debugLabel;  // GPU debug label (for text meshes etc.)
};

// Upload mesh vertex/index data to GPU (creates or re-uploads as needed).
// Returns true if mesh data is ready for drawing.
bool EnsureMeshUploaded(RndMesh* mesh);

// Release GPU resources for a mesh (called from RndMesh destructor).
void CleanupGpuMesh(RndMesh* mesh);

// Set a debug label for GPU buffer names + frame capture.
void SetMeshDebugLabel(RndMesh* mesh, const char* label);

// Set depth bias for a mesh (used by viewer to push combined meshes behind splits).
void SetMeshDepthBias(RndMesh* mesh, int32_t bias);

// Get a mesh's effective label for GPU debugging: debugLabel if set, otherwise Name().
const char* MeshLabel(RndMesh* mesh);

// Get the GPU data entry for a mesh, or nullptr if not uploaded.
GpuMeshData* GetMeshGpuData(RndMesh* mesh);

// Frame statistics — called from BeginDrawing / DrawShowing
void RndMesh_ResetFrameStats();
void IncrementMeshDrawCalls();
int GetMeshFrameCounter();
int GetMeshDrawCallsThisFrame();
