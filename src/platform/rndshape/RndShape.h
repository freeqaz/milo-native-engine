// RndShape.h — per-consumer rndobj "shape" seam for the dc3 GPU backend.
//
// The dc3 backend (WgpuRnd + Mesh_Wgpu/Tex_Wgpu/Part_Wgpu/MaterialSetup/
// MeshGpuCache/TransparentQueue + the rndobj-coupled gfx passes) compiles inside
// the consuming decomp's context, against THAT decomp's rndobj headers. Two
// rndobj generations consume it:
//
//   DC3     dc3-decomp and rb3-xenon: the 2012-era Xbox engine (NgRnd,
//           BaseMaterial, RndCam::GetViewProjectXfms, fat RndMesh::Vert). This
//           is the shape the backend was written against; its seam is a set of
//           identity forwarders, so DC3/xenon code paths are unchanged.
//   RB3WII  rb3 (the Wii decomp): the 2010-era engine. No NgRnd/RndShaderMgr
//           layer, RndMat carries the whole material (no BaseMaterial split),
//           `Rnd* TheRnd`, Color32-packed verts, member-only env/light/cam data.
//
// Every place the backend touches an rndobj surface that DIFFERS between the
// two goes through this header. Surfaces that are identical in both (Name(),
// WorldXfm(), GetColor(), GetDiffuseTex(), Faces(), ...) are used directly.
//
// Selection: the consumer sets MILO_ENGINE_RNDOBJ_SHAPE (CMake; dc3 default),
// which defines MILO_RNDOBJ_SHAPE_RB3WII for the Wii shape.
#pragma once

#include <cstdint>

struct SceneUniforms;   // gfx/UniformStructs.h
class RndTex;
class RndMesh;
class RndMat;

namespace rndshape {
// RB3's Xbox 360 material terms as retail RndShaderStandard::CalcShaderOpts
// selects them (rb3-xenon rndobj/Shader.cpp) and NgMat sets their constants
// (c2 specular, c63 rim, c14 normal-map strength). Filled only by a shape
// whose materials carry them (rndshape::MatRetailTerms); see
// dc3-backend-for-rb3-wii.md section 12.
struct RetailMatTerms {
    bool perPixel = false;      // option bit 0
    bool specular = false;      // bit 2: SpecularRGB().Pack() != 0
    bool specularMap = false;   // bit 1: per-pixel, specular, and a map
    bool normalMap = false;     // bit 5: per-pixel and a map
    bool rim = false;           // bit 37: per-pixel and RimRGB().Pack() != 0
    bool rimMap = false;        // bit 15: rim and a map
    float specular_rgb[3] = {0, 0, 0};
    float specularPower = 0.5f; // c2.w, at least 0.5
    float rim_rgb[3] = {0, 0, 0};
    float rimPower = 0.5f;      // c63.w, at least 0.5
    float deNormal = 0.0f;      // c14.x = 1 - deNormal
    bool rimLightUnder = false; // bit 14
    RndTex *normalTex = nullptr;
    RndTex *specularTex = nullptr;
    RndTex *rimTex = nullptr;
};
// A material's world refraction as retail selects it: RndShaderStandard
// sets option bit 46 (mRefractWorld) when RndMat::GetRefractEnabled holds
// (enabled, strength > 0, a refract normal map) and NgMat puts the strength in
// kPS_RefractStrength (c119). Filled by rndshape::MatRefract; see
// dc3-backend-for-rb3-wii.md section 22.
struct RefractTerms {
    float strength = 0.0f;
    RndTex *normalTex = nullptr;   // RndMat::GetRefractNormalMap
};
// One mesh draw as the per-draw state log records it (rndshape::DrawLogRecord;
// RB3DrawLogDebug.h's RB3DrawRecord is the stored form). Every pointer is
// borrowed for the duration of the call. The four tokens identify the uniform
// data the draw bound (ring buffer + offset; a shared bind group for a static
// mesh's bones), so draws that share uniforms share a token.
struct DrawLogDraw {
    uint64_t pipelineHash = 0;   // PipelineKeyHash of the draw's pipeline key
    uint8_t blend = 0, zMode = 0, layout = 0;
    bool hasDepth = false, alphaCut = false, alphaWrite = false, skinned = false;
    uint32_t targetFormat = 0;
    uint32_t indexCount = 0, triCount = 0, vertCount = 0;
    RndMesh *mesh = nullptr;
    RndMat *mat = nullptr;
    const float *world = nullptr;      // ObjectUniforms.world (column-major)
    const float *viewProj = nullptr;   // SceneUniforms.viewProj (column-major)
    const float *boundColor = nullptr; // MaterialUniforms.color as written
    float viewportW = 0.0f, viewportH = 0.0f;
    uint64_t sceneToken = 0, matToken = 0, objToken = 0, boneToken = 0;
};
} // namespace rndshape

#if defined(MILO_RNDOBJ_SHAPE_RB3WII)
#include "platform/rndshape/RndShape_RB3Wii.h"
#else
#include "platform/rndshape/RndShape_DC3.h"
#endif
