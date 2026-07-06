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

// Forward decls only — this header names no Milo/RB3 concept and includes
// nothing. The per-draw policy methods below receive engine renderer types as
// forward-declared pointers so the header stays game-agnostic (the litmus test
// from the file-scope comment: the ENGINE header must not name an RB3-only
// concept — `RndMesh`/`RndMat`/`RndTransformable` are engine/Milo types, passed
// opaquely; the concrete policy — "does THIS asset name mean X" — lives entirely
// in the game's `<decomp>_render_hook.cpp`).
class RndMesh;
class RndMat;

// ---------------------------------------------------------------------------
// Per-draw policy PODs (W1.7)
// ---------------------------------------------------------------------------
// The two frame-pass methods below cover whole-frame stages. The renderer also
// makes a handful of PER-DRAW decisions that today are hardcoded on RB3 asset/
// material NAME strings inside the engine renderer (`DrawMesh`,
// `RB3BuildMaterialUniforms`, `IsHaloSourceMat`). Those name→decision branches
// are game content policy and must not live in the shared engine. W1.7 factors
// each behavior branch behind the per-draw hook methods below; the engine asks
// the hook for a small POD DECISION and keeps applying the same math it already
// has (so relocated commits stay byte-identical).
//
// Every per-draw method has a base-class no-op default returning "no override"
// (all-false / zero), so a decomp that supplies no policy (or DC3, which routes
// none of these) needs no edit and gets identical prior behavior. The RB3 impl
// (`BandRenderHook`) overrides them with the relocated name matches + the
// existing `RB3_*` env-flag reads.

// Geometric / draw-guard decisions (relocated from `DrawMesh`, B1–B5). The
// engine keeps its matrix math; the hook returns only the DECISION + any
// name-derived scalars so float ordering is never moved across the seam.
struct DrawGeomPolicy {
    // B1: hub highlight-bar world-xfm override (identity + label translation).
    bool  hubBarPlacement = false;
    // B2: scrollbar-thumb reuse of the previous scrollbar-bg draw's world xfm.
    // `scrollbarBg` = the bg mesh (cache the world); `scrollbarThumb` = the
    // thumb mesh (apply the cached world). Mutually exclusive.
    bool  scrollbarBg = false;
    bool  scrollbarThumb = false;
    // B4: shard-guard exemption for hub-bar UI overlay meshes (mesh-name only, so
    // it fits this mesh-scoped POD).
    bool  shardExemptHubBar = false;
    // B5's band-member shard discriminator needs per-bone iteration (the owner's
    // bones), which a mesh-only POD cannot express, so it is answered by the
    // per-string classifier `IsBandMemberSkeletonFile` below rather than a POD
    // field. `shardBandMember` is retained (unused) for source-compat with S1's
    // scaffolding; the live decision is the classifier.
    bool  shardBandMember = false;
    // B3: skel-rebake mesh-level gate == (RB3_NO_SKEL_REBAKE disabled) &&
    // !(dynamic face/hair/fingernail mesh name). The engine ANDs this with the
    // numBones / rebound / worst-bone conditions and keeps all rebake math; the
    // per-bone + dir tests are the classifiers below.
    bool  skelRebakeMesh = false;
};

// B13 highway-material shading classes (the values of
// `DrawMaterialPolicy::highwayClass`). A neutral shared contract: the engine
// applies its per-bucket shading math under its own game-cam gate; the game hook
// maps material names to a bucket. The header names abstract shading buckets, not
// specific asset file names (the name matches live in the game hook). Mutually
// exclusive by construction (the hook maps each material name to exactly one).
enum HighwayMaterialClass {
    kHighwayNone      = 0,
    kHighwaySurface   = 1,   // darken track surface + dim its emissive watermark
    kHighwayRails     = 2,   // force prelit lanes + cool tint
    kHighwaySmasher   = 3,   // boost now-bar/strike emissive
    kHighwayPeakstate = 4,   // brighten SP peak-state overlay
};

// Material-classification decisions (relocated from `RB3BuildMaterialUniforms`
// and `IsHaloSourceMat`, B6–B13). The engine keeps the uniform math; the hook
// returns WHICH class so the uniforms stay bit-identical. Fields default to
// "not classified" so the engine falls through to its prior default path.
struct DrawMaterialPolicy {
    // B7: text/label heuristic (num*/_source.mesh/_comma.mesh/.lbl/font/label)
    // → useAlphaAsRGB / prelit / UI-text colour floor handling.
    bool  isUiText = false;
    // B8: hub highlight bar material colour override.
    bool  isHubHighlight = false;
    // B9: skin-RTT diffuse (`skin_diffuse_output`) handling.
    bool  isSkinRtt = false;
    // B10: colour-icon-font (`icon`) useAlphaAsRGB exclusion.
    bool  isColorIcon = false;
    // B11: tail chain-select material (`tail_` + chain names). `isTailChain` is
    // retained (unused by the engine) for source-compat with S1's scaffolding;
    // the LIVE decision is `tailForceColor` — when a KNOWN fret name matched
    // (tail_{green,red,yellow,blue,orange,purple}.), the engine writes `tailColor`
    // into mu.color[0..2] and sets useTexture=0. The name→colour lookup table lives
    // entirely in the game hook; `tailColor` is a name-derived scalar the engine
    // applies, so float ordering never crosses the seam (tail_white/bonus/star/
    // chord/miss do NOT match and keep the material's own colour).
    bool  isTailChain = false;
    bool  tailForceColor = false;
    float tailColor[3] = {0.f, 0.f, 0.f};
    // B12: crowd/extras vs band-member material path.
    bool  isCrowdExtra = false;
    bool  isBandMember = false;
    // B13: highway per-material shading class (surface/rails/smasher/peakstate
    // under game.cam). One of `HighwayMaterialClass`; kHighwayNone (0) = no
    // highway shading. The engine applies the matching shading math ONLY inside
    // its own sTrackLight + game.cam gate (Bucket-C scene-scope stays inline).
    int   highwayClass = kHighwayNone;
};

// Halo-source exclusion decision (relocated from `IsHaloSourceMat`, B6). The
// engine keeps the emissive-map/multiplier test (that is data, not a name); the
// hook answers only the NAME-based exclusion + the `RB3_SMASHER_HALO` flag.
struct HaloPolicy {
    bool  forceExclude = false;   // name says "never a halo source" (surface / smasher-off)
};

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

    // -----------------------------------------------------------------------
    // Per-draw policy hooks (W1.7). NON-pure — base no-op defaults return "no
    // override" so a decomp that routes none of these (DC3) needs no edit and
    // gets byte-identical prior behavior. RB3's `BandRenderHook` overrides them.
    // -----------------------------------------------------------------------

    // Geometric / draw-guard policy for one mesh draw (B1–B5). The engine calls
    // this in `DrawMesh` with the mesh (and, for the scrollbar-thumb case, the
    // scrollbar-bg world the engine cached from the previous draw is applied by
    // the engine — the hook only signals WHICH decision applies). `outWorld` is
    // an optional 16-float column-major world matrix the hook MAY fill for the
    // hub-bar placement case (identity + label translation); the engine only
    // reads it when `hubBarPlacement` is true. Default: no override.
    virtual DrawGeomPolicy QueryDrawGeomPolicy(RndMesh* /*mesh*/,
                                               float* /*outWorld16*/) {
        return DrawGeomPolicy();
    }

    // Material-classification policy for one mesh/material (B7–B13). `skinned`
    // and `owner` mirror the args the engine already has at the binder call
    // site; `camName` is the active camera's name (passed IN by the engine so
    // the hook never reaches into `RndCam::sCurrent` global state — the Bucket-C
    // camera gate for B12/B13 stays a scene-scope condition the engine owns, but
    // where a classification needs the cam name the engine supplies it). Default:
    // no classification (engine takes its prior default path).
    virtual DrawMaterialPolicy QueryDrawMaterialPolicy(RndMesh* /*mesh*/,
                                                       RndMat* /*mat*/,
                                                       bool /*skinned*/,
                                                       RndMesh* /*owner*/,
                                                       const char* /*camName*/) {
        return DrawMaterialPolicy();
    }

    // Halo-source NAME exclusion policy (B6). The engine keeps the emissive-map
    // /multiplier data test; this answers only the name-based exclusion + the
    // `RB3_SMASHER_HALO` flag. Default: no forced exclusion.
    virtual HaloPolicy QueryHaloPolicy(RndMat* /*mat*/) {
        return HaloPolicy();
    }

    // Per-string name classifiers (W1.7 B3/B5). These answer a single asset-name
    // question about a string the ENGINE already holds while iterating (a bone's
    // owning-dir stored file, or a bone's name), so the engine keeps the loop and
    // the hook owns only the name match. Base defaults return false ("engine's
    // prior default path"); RB3's `BandRenderHook` supplies the real matches.

    // B3/B5: does this bone's owning-dir stored file name the STATIC shared band
    // skeleton (`skeleton_unshared.milo`)? Used for the B3 rebake band-only gate
    // and the B5 shard-guard band-member discriminator.
    virtual bool IsBandMemberSkeletonFile(const char* /*storedFile*/) {
        return false;
    }

    // B3: is this bone part of a per-frame-driven dynamic chain (hair / facial /
    // finger bones) that must be EXCLUDED from the one-time static rebake?
    virtual bool IsRebakeDynamicBone(const char* /*boneName*/) {
        return false;
    }

    // B12: crowd/extras material-path classifiers. The engine keeps the owner-bone
    // loop + the `world.cam` scene-scope gate (Bucket C, inline) and asks the hook
    // only the NAME questions. `IsCrowdExtraMeshName` seeds the crowd/extras flag
    // from a mesh NAME (crowd/extra); `IsCrowdExtraDir` sets it from a bone's
    // owning-dir stored file (char/crowd/ | char/extras/). Band members are still
    // discriminated via `IsBandMemberSkeletonFile` above (shared with B3/B5).
    virtual bool IsCrowdExtraMeshName(const char* /*meshName*/) {
        return false;
    }
    virtual bool IsCrowdExtraDir(const char* /*storedFile*/) {
        return false;
    }

    // -----------------------------------------------------------------------
    // Debug-probe name classifiers (W1.7.S4, Bucket A). The engine's stderr-only
    // diagnostic probes (CAM_DBG, HUB_BAR_PROBE, RB3_HEADMAT_DBG, GEM_VTX/
    // GEM_FORCE, BONE_PROBE, XBONE_TRACK) each gate "should I log/override for
    // THIS mesh/texture" on a hardcoded RB3 asset-name match. These carry zero
    // rendered-output risk in normal operation (the probes are opt-in via env
    // vars that default off; most only fprintf, and none change behavior when
    // their env var is unset), but the asset-name literal itself is still game
    // content that must not live in the shared engine. Each classifier below
    // answers a single yes/no name question; the engine keeps ALL of the
    // probe's actual diagnostic computation/printing/throttling. Base defaults
    // return false (a decomp with no override just never matches, i.e. its
    // probes never fire — matching "no hook registered" behavior).

    // CAM_DBG: is this a highway "key" mesh worth logging camera/NDC info for
    // (prism_gem / gem_smasher / surface)?
    virtual bool IsCamDbgHighwayMesh(const char* /*meshName*/) {
        return false;
    }

    // HUB_BAR_PROBE: is this the focused-menu highlight-bar mesh
    // (highlight_main / highlight_pattern)? Pure name test — independent of the
    // B1/B4 opt-out flags, since the probe should fire regardless of whether
    // those production fixes are enabled.
    virtual bool IsHubBarMesh(const char* /*meshName*/) {
        return false;
    }

    // RB3_HEADMAT_DBG (C8 head-invisible triage): is this the band-member head
    // mesh ("head.mesh")?
    virtual bool IsHeadMesh(const char* /*meshName*/) {
        return false;
    }

    // RB3_HEADMAT_DBG: does this diffuse texture name the composited skin RTT
    // output ("skin_diffuse_output")?
    virtual bool IsSkinDiffuseOutputTex(const char* /*texName*/) {
        return false;
    }

    // GEM_VTX / GEM_FORCE: is this a gem prism mesh ("prism_gem")?
    virtual bool IsGemMesh(const char* /*meshName*/) {
        return false;
    }

    // BONE_PROBE: default outfit-mesh name set used when BONE_PROBE_NAME is not
    // set (plaidshirt / trackjacket / shirt / jacket / vestdenim). The env-var
    // override path (BONE_PROBE_NAME=<substr>) stays in the engine — it carries
    // no baked-in RB3 asset name, just a runtime user-supplied selector.
    virtual bool IsBoneProbeDefaultMesh(const char* /*meshName*/) {
        return false;
    }

    // XBONE_TRACK: is this the trackjacket outfit mesh (the probe's fixed mesh
    // filter, independent of the bone-name selector supplied via the env var)?
    virtual bool IsTrackjacketMesh(const char* /*meshName*/) {
        return false;
    }

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
