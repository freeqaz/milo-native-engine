// The dc3 flavor's WGSL module table (gfx/ShippedWgsl.h). Every module WgpuRnd
// and its passes create with CreateShaderModule, in the order they are built.
// When a pass gains a module, add its accessor here: the validation test can
// only fail on what this table lists.
#include "gfx/ShippedWgsl.h"

const ShippedWgslModule* ShippedWgslModules(int* count) {
    static const ShippedWgslModule kModules[] = {
        {"gfx/standard_wgsl.inc", StandardWgslSource()},
        {"gfx/BloomPass.cpp", BloomPassWgslSource()},
        {"gfx/DofPass.cpp (dof)", DofPassWgslSource()},
        {"gfx/DofPass.cpp (depth resolve)", DofDepthResolveWgslSource()},
        {"gfx/DrawRect2D.cpp", DrawRect2DWgslSource()},
        {"gfx/PostProcPass.cpp", PostProcPassWgslSource()},
        {"gfx/RB3RetailPost.cpp", RB3RetailPostWgslSource()},
        {"gfx/DisplayRamp.cpp", DisplayRampWgslSource()},
        {"gfx/ShadowPass.cpp", ShadowPassWgslSource()},
        {"platform/Part_Wgpu.cpp", ParticleWgslSource()},
    };
    if (count) *count = (int)(sizeof(kModules) / sizeof(kModules[0]));
    return kModules;
}
