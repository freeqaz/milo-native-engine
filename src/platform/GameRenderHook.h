// GameRenderHook — engine-owned interface for game-supplied draw passes.
//
// Rationale
// ---------
// The shared WebGPU renderer (`Rnd_Wgpu.cpp`) used to `#include` DC3 game
// headers (`hamobj/HamDirector.h`, `hamobj/HamCharacter.h`,
// `hamobj/HamGameData.h`) so it could run two game-specific draw stages:
//   (a) a HamDirector "overlay" pass run after post-processing
//   (b) a per-HamCharacter "impostor / render-to-texture" loop
// That coupling blocked the renderer from graduating into the shared engine.
//
// `GameRenderHook` factors those two stages into an abstract interface owned
// by the engine. The engine's renderer calls into the hook for "draw your
// overlay" and "render your impostors" without naming game types. Each decomp
// supplies a concrete implementation:
//
//   DC3 → `dc3-decomp/native/src/dc3_render_hook.cpp` defines `HamRenderHook`,
//         which iterates HamCharacters / dispatches HamDirector draws.
//
//   RB3 → `rb3/native/src/rb3_render_hook.cpp` defines `BandRenderHook`
//         (Phase 0.4 onward; initially a no-op stub).
//
// If no hook is registered (`GetGameRenderHook() == nullptr`), the renderer
// simply skips those stages. The renderer must always null-check.
//
// Interface shape
// ---------------
// Hook methods are kept small and game-agnostic. They take a pointer-sized
// "render context" cookie (currently always the active `WgpuRnd*`, exposed via
// `Rnd_Wgpu.h`) so the hook can call back into engine renderer APIs without
// the engine knowing what the hook needs. The hook implementation is
// responsible for ALL game-type iteration (`ObjDirItr<HamCharacter>`,
// `TheHamDirector`, etc.) — none of that leaks into the engine.
//
// This file deliberately includes no Milo headers; it is C++ + nothing.
// Implementations include whatever game headers they need.

#ifndef MILO_ENGINE_PLATFORM_GAMERENDERHOOK_H
#define MILO_ENGINE_PLATFORM_GAMERENDERHOOK_H

class GameRenderHook {
public:
    virtual ~GameRenderHook() = default;

    // Called by the engine renderer once per frame, AFTER the post-processed
    // venue has been resolved into the framebuffer and a fresh 1x no-depth
    // overlay pass has begun (see `WgpuRnd::FlushPostProcessingForOverlay`).
    // The implementation should issue any game-driven overlay draws (e.g.
    // DC3's `TheHamDirector->Draw()` for the gameplay HUD).
    //
    // `renderCtx` is an opaque cookie — currently always the active `WgpuRnd*`.
    // Implementations cast as needed via the platform header they include.
    virtual void DrawGameOverlay(void* renderCtx) = 0;

    // Called by the engine renderer once per frame, BEFORE the main scene
    // pass, while the encoder is open but no render pass is active. The
    // implementation iterates whatever game objects need to be rendered into
    // off-screen textures (DC3: each `HamCharacter` with an impostor RTT
    // target; RB3: any per-player render-to-texture passes that band gameplay
    // needs). The implementation is responsible for its own iteration and
    // for opening/closing its own render passes via the engine renderer API.
    //
    // `renderCtx` is the same opaque cookie as `DrawGameOverlay`.
    virtual void RenderCharacterImpostors(void* renderCtx) = 0;

protected:
    GameRenderHook() = default;
};

// Engine-side registration. The engine's `Rnd_Wgpu.cpp` calls
// `GetGameRenderHook()` and, if non-null, dispatches into the hook.
// Decomp glue calls `SetGameRenderHook()` at startup (typically via a
// file-scope static initializer in `<decomp>_render_hook.cpp`).
//
// Setting the hook to `nullptr` is supported (testing); passing the same
// pointer twice is idempotent.
void SetGameRenderHook(GameRenderHook* hook);
GameRenderHook* GetGameRenderHook();

#endif  // MILO_ENGINE_PLATFORM_GAMERENDERHOOK_H
