// Part_Wgpu.h — billboard particle drawing (Part_Wgpu.cpp).
#pragma once

#include "math/Color.h"

class RndParticleSys;
class RndMat;
class RndEnviron;

// Draw a particle system's active particles as camera-facing quads.
void DrawParticlesBillboard(RndParticleSys* sys);

// The factor every particle's colour is multiplied by before it is written to
// the vertex buffer, for shapes without the Wii tint model
// (rndshape::kPartMaterialTint false: DC3 and rb3-xenon). It is what retail's
// Xbox `particles` shader computes from its constants:
//
//   VS  mul r0, r2, c1      vertex colour x c1 (kVS_AmbientColor)
//       mul o2, r0, c0      ... x c0 (kVS_Color)
//   PS  mul r0, r0, c5.yyyy texel x c5.y   (option bit 53, intensify only)
//       mul oC0, r0, r2     ... x interpolated colour
//
// NgMat::SetupShader(false, true) (RndShaderParticles::Select) loads c1 with
// RndEnviron::Current()->AmbientColor() when the material uses the environ and
// with white otherwise (NgMat::SetupAmbient), and c0 with the material colour,
// alpha premultiplied for kPreMultAlpha (NgMat::SetRegularShaderConst); c5.y is
// mIntensify + 1. RndShaderParticles::CalcShaderOpts forces the prelit bit, so
// the vertex colour always enters. All three functions are 100% matched.
//
// `env` is the environ current at the draw; null or one without an ambient
// owner counts as white ambient.
Hmx::Color ParticleShaderColor(RndMat* mat, RndEnviron* env);
