// The rb3 flavor's WGSL module table (gfx/ShippedWgsl.h): the standard shader
// (PipelineManager's compiled-in copy) and the five modules BandRnd embeds from
// gfx/Shaders/*.wgsl.inc, included the same compile-time way Rnd_Wgpu_RB3.cpp,
// RB3PostProc, RB3Quad and RB3HaloPass include them, so the table holds the
// exact shipped bytes.
#include "gfx/ShippedWgsl.h"

namespace {
const char* kHaloBlit =
#include "gfx/Shaders/rb3_halo_blit.wgsl.inc"
;
const char* kPostProc =
#include "gfx/Shaders/rb3_postproc.wgsl.inc"
;
const char* kQuad =
#include "gfx/Shaders/rb3_quad.wgsl.inc"
;
const char* kCompose =
#include "gfx/Shaders/rb3_compose.wgsl.inc"
;
const char* kParticle =
#include "gfx/Shaders/rb3_particle.wgsl.inc"
;
} // namespace

const ShippedWgslModule* ShippedWgslModules(int* count) {
    static const ShippedWgslModule kModules[] = {
        {"gfx/Shaders/rb3_halo_blit.wgsl.inc", kHaloBlit},
        {"gfx/Shaders/rb3_postproc.wgsl.inc", kPostProc},
        {"gfx/Shaders/rb3_quad.wgsl.inc", kQuad},
        {"gfx/Shaders/rb3_compose.wgsl.inc", kCompose},
        {"gfx/Shaders/rb3_particle.wgsl.inc", kParticle},
        {"gfx/standard_wgsl.inc", StandardWgslSource()},
    };
    if (count) *count = (int)(sizeof(kModules) / sizeof(kModules[0]));
    return kModules;
}
