#pragma once

class RndMesh;

// The decision RndMesh::DrawShowing (platform/Mesh_Wgpu.cpp) makes before it
// submits a mesh: nullptr when the mesh is drawn, otherwise a short reason
// (the same string FrameCapture records as the skip reason).
//
// Split out so a consumer can test WHICH meshes a DrawShowing pass submits
// without a GPU device or an open render pass -- DrawShowing itself returns at
// its first line when there is no pass, which would make any such test vacuous.
const char* RndMeshDrawShowingSkip(RndMesh* mesh);
