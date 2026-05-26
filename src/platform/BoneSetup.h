#pragma once

#include "platform/Rnd_Wgpu.h"

class RndMesh;

// Fill bone matrices from mesh's bone transforms into a BoneUniforms struct.
// Iterates mesh bones, computes skin matrices via Multiply(BoneOffsetAt, WorldXfm),
// converts to mat4, and fills identity for remaining slots.
void FillBoneUniforms(RndMesh* mesh, BoneUniforms& out);

// Ensure the dummy bone bind group exists (for static meshes that still
// need a group 3 binding to satisfy the pipeline layout).
void EnsureDummyBoneBindGroup();

// Get the dummy bone bind group (identity matrices).
wgpu::BindGroup GetDummyBoneBindGroup();
