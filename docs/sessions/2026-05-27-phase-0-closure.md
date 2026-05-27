# Phase 0 closure — engine extracted, both decomps converged (2026-05-27)

## Summary

**Phase 0 of the native-port roadmap is COMPLETE.** `milo-native-engine` now
exists as a buildable sibling repo, is consumed by both Milo decomps, and is
gated by two convergence test suites. This session log closes out substeps
0.1 → 0.2 → 0.2a → 0.2b → 0.3 (milestone a) → 0.4.

The engine is a game-agnostic, SDK-agnostic LP64 modern-C++ runtime. Each
decomp links it via `add_subdirectory` with a soft SHA pin and assembles its
own executable from engine + matched-fork + per-decomp glue. The canonical plan
lives in `../rb3/docs/native/NATIVE_PORT_ROADMAP.md`; this log is the engine-side
mirror of dc3's `docs/sessions/` pattern.

## Milestones closed

| Substep | What landed |
|---------|-------------|
| **0.1** | Engine repo skeleton — game-agnostic `CMakeLists.txt` builds a near-empty `libmilo-engine.a` from an `EngineVersion` placeholder TU; README, `.gitignore`, `src/` subsystem tree, `docs/`. dc3-decomp untouched. |
| **0.2** | Engine-clean source (gfx / audio / char / clean-platform = 47 TUs) + vendored headers extracted out of `dc3-decomp/native/`. Engine gained a **consumer-injected context** model (`MILO_ENGINE_DECOMP_INCLUDE_DIRS` / `_COMPAT_FLAGS` / `_PCH`) so the same engine source compiles against the consuming decomp's Milo headers + matched-fork compat flags. All four DC3 consumers — `dc3-native`, `milo-viewer`, `render-test`, `milo-tests` — switched to link `libmilo-engine.a`. |
| **0.2a** | `Rnd_Wgpu` graduated into the engine behind a clean `GameRenderHook` interface (`src/platform/`): `DrawGameOverlay` / `RenderCharacterImpostors`, opaque `void*` cookie. DC3 supplies `HamRenderHook` via static auto-registration; RB3 will supply `BandRenderHook`. (Finding: the historical `hamobj/` call sites were already removed in dc3 commit `a97fbac6`, leaving orphan includes — the seam codifies the now-clean state.) |
| **0.2b** | `Memory_Native` + `ThreadCall_Native` graduated with POSIX-only impls (pthread + `sem_t`); they no longer `#include "xdk/XAPILIB.h"` and use no Win32 type names. Engine sources 47 → 49, then → 50 with `Rnd_Wgpu`/`GameRenderHook`. DC3's matched fork keeps `xdk_shims.cpp` for the non-`os/` Win32 calls. |
| **0.3 (a)** | `rb3/native/` stood up consuming `milo-engine` via `add_subdirectory` with **no injected decomp context** (`MILO_ENGINE_HAVE_CONTEXT=OFF` → engine builds placeholder lib only; gfx/Dawn deferred to Phase 2). Headless `rb3-dta` parses **138 real RB3 songs** from arkhelper-extracted assets (`orig-assets/extracted/songs/songs.dta`), exit 0 — proof the matched-fork DTA path runs clean under clang LP64. |
| **0.4** | Per-repo GitHub Actions workflows (engine + dc3 + rb3) for native build + test, pinned Dawn release + asset-skip behavior, actionlint-clean. |

## Convergence metrics

- **`milo-tests` (DC3, links `libmilo-engine.a`): 371/371 pass** — maintained
  green through the entire extraction/refactor.
- **`milo-engine-tests` (in-engine convergence gate): 195/195 pass** + 1
  intentional skip. Ran 3× consecutively at 195/195. This grew from an earlier
  162-test set after `test_asset_loading` rejoined (see below).
- **`rb3-dta`: 138/138 songs** parsed from real extracted RB3 assets.

### `milo-engine-tests` transitional link model

Until the Milo CORE (`utl`/`obj`/`math`/`rndobj`/`os`/`synth`) graduates into
the engine in a future phase, `milo-engine-tests` links `libmilo-engine.a` plus
DC3's reference core via a generated `tests/dc3_runtime_sources.cmake`
(read-only consumption of the dc3 checkout). That list shrinks toward empty as
core graduates.

### `test_asset_loading` rejoin

The previously-dropped `test_asset_loading` re-entered the suite (162 → 195
tests). The original "GpuDevice/Wgpu atexit teardown" hypothesis was **wrong**;
the real bug was a `nullptr TheUI` deref in `PanelDir::SendTransition`, reached
via `HamDirector::OnFileMerged` during a song-milo load. Fix: install a
default-constructed `UIManager` stub as `TheUI` in `EnsureEngineInit()`.

## Pin history

| Pin | Phase / state |
|-----|---------------|
| `291a70f` | 0.2 — DC3 first converges onto extracted engine; `milo-tests` 371/371. |
| `7b5adf5` | 0.2 — `milo-engine-tests` convergence gate added (then 161/162). |
| `015f62f` | 0.4 — engine-tests CI workflow. |
| `9ec8723` | 0.2b — `Memory_Native` + `ThreadCall_Native` graduate (drop xdk). |
| `4824217` | asset-loading fix (TheUI nullptr) — suite rejoins. |
| **`9a58e86`** | **0.2a — `Rnd_Wgpu` behind `GameRenderHook`. Current pin in both decomps.** |

Published at <https://github.com/freeqaz/milo-native-engine> (public).

## Deferred per-decomp glue

These matched-fork interface headers still pull SDK/game couplings; they await
HX_NATIVE-gating before graduating into the engine:

- **`PlatformMgr_Native`** — `xdk/XSOCIAL`.
- **`RenderState_Native`** — `xdk/D3D9`.
- **`Skeleton_Native`** — Kinect.
- **`HttpServer` + `DebugPanel`** — telemetry / `DC3_HTTP_SERVER`.
- **`MeshFilter`** — DC3 hardcoded Kinect mesh skip list.

## Follow-ups

1. **rb3 milestone (b)** — flip `rb3-native` to inject decomp context
   (`MILO_ENGINE_HAVE_CONTEXT=ON`, full MWCC context injection), link the whole
   engine, run to a controlled exit, then Phase 1 `.milo` scene-tree dump.
   (In progress this session by other agents.)
2. **Engine-vs-dc3 link-order divergence (under investigation)** — the engine
   standalone binary reaches `OnFileMerged` reliably; dc3's `milo-tests` binary
   doesn't. A real Phase-0.2 link-order divergence under
   `-Wl,--allow-multiple-definition`. Needs root-causing.
3. **Graduate the 5 deferred-glue files** as RB3/DC3 exercise them and the
   couplings can be HX_NATIVE-gated.

## Reference

- Canonical roadmap: `../rb3/docs/native/NATIVE_PORT_ROADMAP.md`
- Inventory / disposition catalog: `../rb3/docs/native/NATIVE_PORT_INVENTORY.md`
- DC3 native status: `../dc3-decomp/docs/native/NATIVE_PORT_STATUS.md`
- rb3 native tool: `../rb3/native/README.md`
