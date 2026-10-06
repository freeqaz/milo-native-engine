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

#if defined(MILO_RNDOBJ_SHAPE_RB3WII)
#include "platform/rndshape/RndShape_RB3Wii.h"
#else
#include "platform/rndshape/RndShape_DC3.h"
#endif
