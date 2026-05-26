// DC3 Native Port - Renderer Stub
// Replaces system/rnddx9/*.cpp - provides NativeRnd extending NgRnd
// Eventually WebGPU via Dawn; for now, headless stubs for engine boot.

#include "rndobj/Rnd_NG.h"
#include "rndobj/Cam.h"
#include "rndobj/Env.h"
#include "rndobj/HiResScreen.h"
#include "rndobj/Overlay.h"
#include "rndobj/PostProc.h"
#include "rndobj/ShaderMgr.h"
#include "ui/UI.h"
#include <cstdio>

// ============================================================================
// NativeShaderMgr - stub for all GPU shader operations
// ============================================================================
class NativeShaderMgr : public RndShaderMgr {
public:
    NativeShaderMgr() {}
    virtual ~NativeShaderMgr() {}

    void Init() override {}
    void Terminate() override {}

    void SetVConstant(VShaderConstant, const Hmx::Matrix4 &) override {}
    void SetVConstant4x3(VShaderConstant, const Hmx::Matrix4 &) override {}
    void SetVConstant(VShaderConstant, RndTex *) override {}
    void SetVConstant(VShaderConstant, const Vector4 &) override {}
    void SetVConstant(VShaderConstant, const float *, unsigned int) override {}
    void SetVConstant(VShaderConstant, int) override {}
    void SetVConstant(VShaderConstant, bool) override {}
    void SetPConstant(PShaderConstant, const Hmx::Matrix4 &) override {}
    void SetPConstant(PShaderConstant, RndCubeTex *) override {}
    void SetPConstant(PShaderConstant, const Vector4 &) override {}
    void SetPConstant(PShaderConstant, RndTex *) override {}
    void SetPConstant(PShaderConstant, int) override {}
    void SetPConstant(PShaderConstant, bool) override {}
    void SetPConstant4x3(PShaderConstant, const Hmx::Matrix4 &) override {}

protected:
    RndShaderProgram *NewShaderProgram() override { return nullptr; }
};

// ============================================================================
// NativeRnd - headless renderer for engine boot
// ============================================================================
class NativeRnd : public NgRnd {
public:
    NativeRnd() {}
    virtual ~NativeRnd() {}

    // Pure virtual from Rnd
    void Clear(unsigned int, const Hmx::Color &) override {}

    // Initialize rendering subsystem registrations without D3D
    void Init() override {
        printf("DC3 Native: NativeRnd::Init() - headless mode\n");
        PreInit();  // Registers subsystem types, creates default cam/env/mat/etc.
    }

    void Terminate() override {}

    // Headless drawing - no actual rendering
    void BeginDrawing() override {
        mDrawing = true;
        mWorldEnded = false;
        mDrawCount++;
        mFrameID++;
    }

    void EndDrawing() override {
        mDrawing = false;
    }
};

// ============================================================================
// Global instances and references
// ============================================================================
static NativeShaderMgr gNativeShaderMgr;
static NativeRnd gNativeRnd;

Rnd &TheRnd = gNativeRnd;
NgRnd &TheNgRnd = gNativeRnd;
RndShaderMgr &TheShaderMgr = gNativeShaderMgr;

// UIManager - starts null, created during subsystem init
UIManager *TheUI = nullptr;
