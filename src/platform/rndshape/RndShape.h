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

struct SceneUniforms;   // gfx/UniformStructs.h
class RndTex;

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
} // namespace rndshape

#if defined(MILO_RNDOBJ_SHAPE_RB3WII)
#include "platform/rndshape/RndShape_RB3Wii.h"
#else
#include "platform/rndshape/RndShape_DC3.h"
#endif
