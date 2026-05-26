// DC3 Native Port — Material Setup
// Extracts material uniform filling and texture resolution from Mesh_Wgpu.cpp
// into reusable functions for primary and multi-pass draw calls.

#pragma once

#include "platform/Rnd_Wgpu.h"
#include <cstdint>

class RndMat;
class BaseMaterial;

// All material-related data needed for a draw call
struct MaterialParams {
    MaterialUniforms uniforms;
    WgpuRnd::MaterialTexViews texViews;
    SamplerDesc samplerDesc;
    SamplerDesc mapSamplerDesc;
    uint32_t heuristics;
};

// Build material parameters for a primary material (full heuristics)
MaterialParams BuildMaterialParams(RndMat* mat, bool isTextMesh);

// Build material parameters for a multi-pass material (simplified, no heuristics)
MaterialParams BuildPassMaterialParams(BaseMaterial* pass);
