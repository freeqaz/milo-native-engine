#pragma once
//
// The WGSL modules the linked GPU backend flavor hands Dawn, as the exact
// source strings it compiles at runtime. A test can compile every one of them
// against the real device and fail on any WGSL error before a frame draws
// (rb3 native/tests/test_wgsl_validation.cpp).
//
// Exactly one definition links, chosen by MILO_ENGINE_GPU_BACKEND:
//   dc3  gfx/ShippedWgsl_DC3.cpp     the standard shader + every WgpuRnd pass
//   rb3  platform/RB3ShippedWgsl.cpp the standard shader + BandRnd's five modules
//
// A flat array, not a std::vector: the engine's platform TUs compile inside the
// consumer's include context, where RB3's matched-fork STLport headers can
// shadow the host standard library.

struct ShippedWgslModule {
    const char* name;   // where the source lives, for failure messages
    const char* code;   // the WGSL source, NUL-terminated
};

// Returns the module table and writes its length to *count.
const ShippedWgslModule* ShippedWgslModules(int* count);

// The dc3 flavor's per-pass sources, each defined in the file that compiles it.
const char* StandardWgslSource();      // gfx/PipelineManager.cpp (compiled-in copy)
const char* BloomPassWgslSource();     // gfx/BloomPass.cpp
const char* DofPassWgslSource();       // gfx/DofPass.cpp, depth of field
const char* DrawRect2DWgslSource();    // gfx/DrawRect2D.cpp
const char* PostProcPassWgslSource();  // gfx/PostProcPass.cpp
const char* RB3RetailPostWgslSource(); // gfx/RB3RetailPost.cpp
const char* DisplayRampWgslSource();   // gfx/DisplayRamp.cpp
const char* PointTestPassWgslSource(); // gfx/PointTestPass.cpp, flare occlusion queries
const char* ShadowPassWgslSource();    // gfx/ShadowPass.cpp
const char* ParticleWgslSource();      // platform/Part_Wgpu.cpp
