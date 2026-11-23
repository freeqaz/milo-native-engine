#pragma once
// TextGlyphMarks -- which meshes are RndText glyph pages.
//
// The game's text code marks each page mesh it creates (MarkTextGlyphMesh); the
// renderer asks IsMarkedTextGlyphMesh() when the game says it marks its pages
// (GameMarksTextGlyphMeshes, platform/MeshGpuCache.h; MeshDrawsAsGlyph,
// platform/MaterialSetup.h). The side table lives in its own dependency-free TU
// so that a consumer which links no engine (rb3-xenon's rb3-milo) can compile
// just this file for the game code that marks pages, as it does PointTestHook.cpp.
// It is kept apart from the GPU buffer entries, so ClearMeshGpuCache() does not
// drop a mark; CleanupGpuMesh() (the mesh is being destroyed) does, through
// UnmarkTextGlyphMesh().

class RndMesh;

void MarkTextGlyphMesh(RndMesh* mesh);
bool IsMarkedTextGlyphMesh(RndMesh* mesh);
void UnmarkTextGlyphMesh(RndMesh* mesh);
