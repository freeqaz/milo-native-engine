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

// Invalidate the cached upload for a mesh so the next EnsureMeshUploaded()
// re-reads from mesh->GetGeomOwner(). Used when SetGeomOwner() swaps in a
// different source RndMesh (e.g. BandScoreboard digit-mesh hot-swap from
// `num%d.mesh` -> `%d_source.mesh`). Without this, the cache returns the
// already-uploaded geometry from the previous owner and the digit slot
// keeps rendering whatever it had before (typically nothing).
void InvalidateGpuMesh(RndMesh* mesh);

// Clear all entries in the GPU mesh cache. Used by hosts that want to drop
// every wgpu::Buffer ref the cache is holding before the underlying device /
// Vulkan ICD is torn down (e.g. RB3 native's BandRnd::Shutdown exit callback,
// which fires from Debug::Exit ahead of libc's exit() static destructors — by
// the time libc tears statics down, the Vulkan ICD .so is unmapped and any
// surviving wgpu::~ObjectBase that drops the last ref crashes on a dangling
// vkDestroy* pointer).
void ClearMeshGpuCache();

// Set a debug label for GPU buffer names + frame capture.
void SetMeshDebugLabel(RndMesh* mesh, const char* label);

// Mark a mesh as an RndText glyph page (called by the game's text code where it
// creates the page mesh). Read through IsMarkedTextGlyphMesh(); honoured only
// when the render hook's MarksTextGlyphMeshes() says the game marks its pages
// (MeshDrawsAsGlyph, platform/MaterialSetup.h). Kept apart from the GPU buffer
// entries so ClearMeshGpuCache() does not drop it; CleanupGpuMesh() (the mesh
// is being destroyed) does.
void MarkTextGlyphMesh(RndMesh* mesh);
bool IsMarkedTextGlyphMesh(RndMesh* mesh);

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
