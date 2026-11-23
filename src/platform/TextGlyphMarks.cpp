// TextGlyphMarks -- engine-side storage for the glyph-page marks. See
// TextGlyphMarks.h.

#include "platform/TextGlyphMarks.h"

#include <unordered_set>

namespace {
std::unordered_set<RndMesh*> sTextGlyphMeshes;
}

void MarkTextGlyphMesh(RndMesh* mesh) {
    if (mesh) sTextGlyphMeshes.insert(mesh);
}

bool IsMarkedTextGlyphMesh(RndMesh* mesh) {
    return mesh && sTextGlyphMeshes.count(mesh) != 0;
}

void UnmarkTextGlyphMesh(RndMesh* mesh) { sTextGlyphMeshes.erase(mesh); }
