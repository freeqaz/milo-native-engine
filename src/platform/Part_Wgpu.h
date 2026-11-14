// Part_Wgpu.h — billboard particle drawing (Part_Wgpu.cpp).
#pragma once

#include "math/Color.h"

class RndParticleSys;
class RndMat;
class RndEnviron;

// Draw a particle system's active particles as camera-facing quads.
void DrawParticlesBillboard(RndParticleSys* sys);

// One particle's quad: corner = centre +- right +- up (TL = centre - right + up).
// camRight/camUp are the camera's world X and Z axes; halfSize is size / 2.
// retailXbox selects the shipped Xbox particle shader's billboard
// (rndshape::PartBillboard, DC3 shape): up scaled by screenAspect, a
// clockwise turn by angle, and the centre moved swingArm along the turned up
// axis. Otherwise the engine's original billboard: the angle turns the quad
// anticlockwise, and screenAspect and swingArm are ignored.
struct ParticleQuadAxes {
    float centre[3];
    float right[3];
    float up[3];
};
ParticleQuadAxes ParticleBillboardAxes(const float pos[3], const float camRight[3],
                                       const float camUp[3], float halfSize, float angle,
                                       bool retailXbox, float screenAspect, float swingArm);

// The shipped particle shader's velocity-aligned quad (DC3 shape,
// rndshape::PartBillboard aligned). `pos` is the particle's world position,
// `velLocal` its velocity in the relative space whose matrix rows are `rel`
// (p->vel as DrawParticles reads it), `size` the full particle size. The quad's
// top edge (uv v0, the "+up" side) leads along the screen-projected velocity
// and its bottom edge sits on the particle; a particle whose velocity projects
// to nothing collapses to a point, as on the console.
struct ParticleAlignTerms {
    float screenAspect = 1.0f;
    bool stretched = false;
    bool constantArea = false;
    bool perspective = false;
    float stretchScale = 1.0f;
};
ParticleQuadAxes ParticleAlignedAxes(const float pos[3], const float velLocal[3],
                                     const float rel[9], const float camRight[3],
                                     const float camUp[3], float size,
                                     const ParticleAlignTerms& terms);

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
