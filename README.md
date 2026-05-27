# milo-native-engine

Shared, cross-platform **LP64 modern-C++ runtime** for native ports of
Milo-engine games. It is the deliverable engine that the decomp repos link
against to produce playable native builds:

- **dc3-decomp** — Dance Central 3 (Xbox 360, MSVC-PPC). Mature native port.
- **rb3** — Rock Band 3 (Wii, MWCC). First bring-up target.
- **rb3-xenon** — Rock Band 3 (Xbox 360, MSVC-PPC). In progress.

The engine is **game-agnostic and SDK-agnostic**. It never includes per-game
headers (`hamobj/`, `band3/`, `dance/`, `meta_*`) or per-platform SDK headers
(`xdk/`, `revolution/`). It owns the parts that are the same for every port:
WebGPU graphics, miniaudio/FFmpeg audio, input, file I/O, the host-STL shim
layer, POSIX implementations of the `os/` interfaces, and the CMake scaffolding.

> **Canonical plan:** [`rb3/docs/native/NATIVE_PORT_ROADMAP.md`](../rb3/docs/native/NATIVE_PORT_ROADMAP.md)
> **What moves here, in what shape:** [`rb3/docs/native/NATIVE_PORT_INVENTORY.md`](../rb3/docs/native/NATIVE_PORT_INVENTORY.md)

## Three-layer source model

Every native source file belongs to exactly one layer. Be explicit about which:

| Layer | Lives in | Compiles under | Role |
|-------|----------|----------------|------|
| **Matched fork** | `<decomp>/src/system/**` | MWCC (RB3) / MSVC-PPC (DC3) | Asm-match verification. **Off** the native link line. |
| **Engine runtime** | `milo-native-engine/src/**` | Clang LP64 only | The deliverable. Linked by every decomp's native build. |
| **Per-decomp glue** | `<decomp>/native/src/**` | Clang LP64 only | Compat shims (`mwcc_compat.h` / `msvc_compat.h`), SDK shims (`rvl_shims.cpp` / `xdk_shims.cpp`), link glue, game-specific stubs. |

The matched fork and the engine runtime are *separate copies* that drift
independently. They share **header interfaces** (e.g. `os/ThreadCall.h`) but
compile separate `.cpp` files; they are never merged.

## How a decomp consumes the engine

The engine is a **sibling repo**, not a submodule. Each decomp's top-level
`CMakeLists.txt` pulls it in by path with a **soft SHA pin** (mismatch warns,
never fails) and assembles its own executable:

```cmake
set(MILO_ENGINE_PATH "${CMAKE_SOURCE_DIR}/../milo-native-engine" CACHE PATH "")
set(MILO_ENGINE_PIN  "<commit-sha>" CACHE STRING "")
# ... compare HEAD vs pin, message(WARNING) on mismatch ...
add_subdirectory(${MILO_ENGINE_PATH} ${CMAKE_BINARY_DIR}/milo-engine)

add_executable(rb3-native ${RB3_NATIVE_GLUE_SOURCES} ${RB3_MATCHED_SOURCES})
target_link_libraries(rb3-native PRIVATE milo-engine)
target_compile_definitions(rb3-native PRIVATE HX_NATIVE=1)
```

There is no `MILO_NATIVE_GAME=DC3|RB3` switch inside the engine. The engine
knows nothing about either game; each decomp links it plus its own sources.

## Build (standalone skeleton)

```sh
cmake -B build
cmake --build build        # produces build/libmilo-engine.a
```

The engine targets **Clang LP64 C++17**. Standalone configure autodetects
`clang`/`clang++`. Useful options:

| Option | Default | Effect |
|--------|---------|--------|
| `MILO_ENGINE_BUILD_TOOLS` | `OFF` | Build `milo-viewer`, `milo2gltf`, `render-test` (Phase 0.2). |
| `MILO_ENGINE_BUILD_TESTS` | `ON` standalone | Engine-only convergence test suite (Phase 0.2). |
| `MILO_BUILD_WEB` | `OFF` | Emscripten/web target machinery (Phase 6). |
| `MILO_ENGINE_ENABLE_ASAN` | `OFF` | AddressSanitizer. |
| `MILO_ENGINE_LP64_AUDIT` | `OFF` | Pointer-truncation warnings. |

## Layout

```
milo-native-engine/
├── CMakeLists.txt        engine-only; per-game source lists live in each decomp
├── cmake/                helper modules
├── include/              vendored single-headers (cgltf, stb, httplib, ...)
├── third_party/          vendored libs (miniaudio.h) — no ncnn
├── src/
│   ├── gfx/              WebGPU device, pipelines, post-processing, shadow
│   ├── audio/            miniaudio device + web adapter
│   ├── platform/         file I/O, input, pthread threading, renderer glue
│   ├── char/             engine character helpers (CharTwistSolver)
│   ├── stl/              host-STL shim layer (the STL ABI seam)
│   ├── system/           NEW clean-LP64 impls of os/ interfaces
│   ├── export/           glTF/material/texture exporters (tool target)
│   ├── tools/            CLI tools (opt-in)
│   ├── viewer/           standalone milo viewer (opt-in)
│   └── render_test/      programmatic render test (opt-in)
├── tests/                engine-only convergence test subset
└── docs/                 engine docs + session notes (mirrors decomp layout)
```

Subsystem trees are empty in Phase 0.1 and populated by the Phase 0.2
extraction from `dc3-decomp/native/`.

## Status

**Phase 0 — COMPLETE.** The engine is extracted, consumed by both decomps, and
gated by two convergence test suites. Substeps closed:

- **0.1** — engine repo skeleton; valid near-empty `libmilo-engine.a`.
- **0.2** — engine-clean code (gfx/audio/char/clean-platform) extracted out of
  `dc3-decomp/native/`; DC3 switched to consume this repo via a
  consumer-injected context model; engine-only tests moved in as
  `milo-engine-tests`.
- **0.2a** — `Rnd_Wgpu` graduated into the engine behind a game-agnostic
  `GameRenderHook` interface (`src/platform/`); DC3 supplies `HamRenderHook`,
  RB3 will supply `BandRenderHook`.
- **0.2b** — `Memory_Native` + `ThreadCall_Native` graduated with POSIX-only
  impls (pthread + `sem_t`); they no longer `#include "xdk/XAPILIB.h"`.
- **0.3** — `rb3/native/` stood up; headless `rb3-dta` parses 138 real RB3 songs
  from arkhelper-extracted assets (milestone (a)).
- **0.4** — per-repo CI workflows (engine + dc3 + rb3) for native build + test.

The engine now owns **gfx / audio / char / clean-platform = 50 TUs** (was 47;
+`Memory_Native`, +`ThreadCall_Native`, +`Rnd_Wgpu`/`GameRenderHook`), plus the
`GameRenderHook` interface in `src/platform/`.

**Deferred per-decomp glue** — matched-fork interface headers that still pull
SDK/game couplings; they graduate once HX_NATIVE-gated:
`PlatformMgr_Native` (xdk/XSOCIAL), `RenderState_Native` (xdk/D3D9),
`Skeleton_Native` (Kinect), `HttpServer`+`DebugPanel`
(telemetry/`DC3_HTTP_SERVER`), `MeshFilter` (DC3 hardcoded Kinect mesh skip
list).

**Convergence gates:**
- DC3's `milo-tests` (links `libmilo-engine.a`): **371/371 pass.**
- In-engine `milo-engine-tests`: **195/195 pass** (1 intentional skip).

Published at <https://github.com/freeqaz/milo-native-engine> (public). Both
decomps pin engine `9a58e86aa41e22a9fb90b4af39675eefd09a717e`. See the roadmap's
[Status Log](../rb3/docs/native/NATIVE_PORT_ROADMAP.md#status-log) for the live
phase tracker.
