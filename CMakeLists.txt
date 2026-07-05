cmake_minimum_required(VERSION 3.22)

# =============================================================================
# milo-native-engine
#
# Shared LP64 modern-C++ runtime for native ports of Milo-engine games
# (Dance Central 3, Rock Band 3 Wii, Rock Band 3 Xbox 360). GAME-AGNOSTIC and
# SDK-AGNOSTIC: no per-game classes (hamobj/, band3/, dance/, meta_*), no
# per-platform SDK headers (xdk/, revolution/).
#
# Canonical plan: rb3/docs/native/NATIVE_PORT_ROADMAP.md
# Disposition catalog: rb3/docs/native/NATIVE_PORT_INVENTORY.md
#
# --- Build model -------------------------------------------------------------
# The engine .cpp files include the consuming decomp's Milo HEADERS (obj/,
# rndobj/, math/, synth/, char/ ...) and are compiled with that decomp's
# matched-fork compat flags (MSVC for DC3/Xenon, MWCC for RB3-Wii). So the
# engine is NOT standalone-compilable: it compiles inside a consumer's context.
#
# A consumer (a decomp's CMakeLists, or a standalone "reference" build via -D)
# injects context BEFORE add_subdirectory():
#     MILO_ENGINE_DECOMP_INCLUDE_DIRS  decomp Milo header roots
#     MILO_ENGINE_DECOMP_COMPAT_FLAGS  matched-fork compat flags
#     MILO_ENGINE_DECOMP_DEFINITIONS   extra compile definitions
#     MILO_ENGINE_DECOMP_PCH           optional decomp PCH header
#
# With context, milo-engine compiles the full gfx/audio/platform/char source
# set. WITHOUT context, it builds the Phase-0.1 placeholder-only near-empty
# archive (the skeleton), so `cmake -B build && cmake --build build` still works
# bare.
# =============================================================================

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MILO_ENGINE_STANDALONE ON)
else()
    set(MILO_ENGINE_STANDALONE OFF)
endif()

# Snapshot the engine source dir so helpers defined in this file can refer to
# engine-relative paths from inside a CMake function() body. CMake's
# CMAKE_CURRENT_SOURCE_DIR resolves to the CALLER's source dir when used inside
# a function (DC3's native/, RB3's native/, …), so engine assets resolved that
# way disappear. Stash the engine's own source dir into an INTERNAL cache var
# at top-level (where CMAKE_CURRENT_SOURCE_DIR == the engine repo root) and
# reference it from the helper instead.
set(MILO_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL
    "milo-native-engine source root (snapshot for use inside helper functions)")

# Clang autodetect when built standalone (consumers select their own compiler).
if(MILO_ENGINE_STANDALONE AND NOT EMSCRIPTEN)
    if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
        find_program(CLANG_C_COMPILER clang)
        if(CLANG_C_COMPILER)
            set(CMAKE_C_COMPILER "${CLANG_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
        endif()
    endif()
    if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
        find_program(CLANG_CXX_COMPILER clang++)
        if(CLANG_CXX_COMPILER)
            set(CMAKE_CXX_COMPILER "${CLANG_CXX_COMPILER}" CACHE FILEPATH "C++ compiler" FORCE)
        endif()
    endif()
endif()

project(milo-engine LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "milo-engine targets Clang LP64 (found ${CMAKE_CXX_COMPILER_ID}). "
        "The decomp builds that consume this engine require Clang.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(MILO_ENGINE_BUILD_TOOLS  "Build engine CLI tools (milo-viewer, milo2gltf, render-test)" OFF)
option(MILO_BUILD_WEB           "Build the Emscripten/web target machinery"                     OFF)
# GPU backend toggle. ON (default) → DC3's full WebGPU gfx + Wgpu platform
# backends build, and the heavy GPU deps (Dawn/glfw/imgui) are pulled in. OFF →
# the engine drops src/gfx/* and the GPU-coupled platform TUs and skips the GPU
# deps entirely. RB3-Wii sets this OFF: its 2010-era rndobj/ is structurally
# older than DC3's and cannot compile against the DC3-wired WebGPU layer (see
# the excluded-source partition below). A .milo scene-tree dump needs no GPU,
# so RB3 still reaches the load/dump path with this OFF.
option(MILO_ENGINE_BUILD_GFX    "Build the rndobj-FREE WebGPU gfx core (GpuDevice, pipelines, screenshot, headless frame, …) + Dawn/glfw/imgui deps" ON)
# -----------------------------------------------------------------------------
# GPU backend FLAVOR. The rndobj-FREE gfx core above (MILO_ENGINE_BUILD_GFX) is
# game-agnostic and shared by every consumer. The rndobj-COUPLED backend on top
# of it is NOT — it reads a specific decomp's rndobj/ shapes. Pick which one:
#
#   off  no rndobj-coupled gfx (header-dump / clear-frame smoke only)
#   dc3  WgpuRnd : NgRnd  + the 6 rndobj-coupled gfx TUs (DrawRect2D/DofPass/
#        VertexFormats/ShadowPass/PostProcPass/TextureConvert) + 8 Wgpu platform
#        backends. Reads DC3's 2012-era rndobj (RndMat/RndCam/RndMesh::Vert/
#        RndPostProc/RndBitmap, WgpuRnd derives from NgRnd).
#   rb3  BandRnd : Rnd (Rnd_Wgpu_RB3.cpp). Reads RB3's older 2010-era rndobj
#        (no NgRnd; RndCam without GetViewProjectXfms; packed Color32 verts).
#        Both backends share gfx/UniformStructs.h + gfx/VertexFormats.h (the
#        decomp-agnostic shader contract). The 6 rndobj-coupled gfx TUs are
#        DC3-only for now (their RB3 variants are deferred — see roadmap Phase 2).
#
# A consumer that builds gfx but names no backend gets `dc3` (back-compat: the
# pre-split single-backend build == dc3). DC3 sets dc3 explicitly; RB3 sets rb3.
#
# Back-compat: the retired bool MILO_ENGINE_BUILD_GPU_BACKENDS maps onto the new
# string (ON -> dc3, OFF -> off) with a deprecation warning, so a stale consumer
# CMakeLists keeps working until it migrates.
if(DEFINED MILO_ENGINE_BUILD_GPU_BACKENDS AND NOT DEFINED MILO_ENGINE_GPU_BACKEND)
    if(MILO_ENGINE_BUILD_GPU_BACKENDS)
        set(_milo_compat_backend dc3)
    else()
        set(_milo_compat_backend off)
    endif()
    message(WARNING
        "MILO_ENGINE_BUILD_GPU_BACKENDS is deprecated; set "
        "MILO_ENGINE_GPU_BACKEND={off,dc3,rb3} instead "
        "(mapped '${MILO_ENGINE_BUILD_GPU_BACKENDS}' -> '${_milo_compat_backend}').")
    set(MILO_ENGINE_GPU_BACKEND ${_milo_compat_backend} CACHE STRING "GPU backend flavor" FORCE)
endif()
if(NOT DEFINED MILO_ENGINE_GPU_BACKEND)
    if(MILO_ENGINE_BUILD_GFX)
        set(MILO_ENGINE_GPU_BACKEND dc3 CACHE STRING "GPU backend flavor: off, dc3, rb3")
    else()
        set(MILO_ENGINE_GPU_BACKEND off CACHE STRING "GPU backend flavor: off, dc3, rb3")
    endif()
endif()
set_property(CACHE MILO_ENGINE_GPU_BACKEND PROPERTY STRINGS off dc3 rb3)
if(NOT MILO_ENGINE_GPU_BACKEND MATCHES "^(off|dc3|rb3)$")
    message(FATAL_ERROR
        "MILO_ENGINE_GPU_BACKEND must be off, dc3, or rb3 (got '${MILO_ENGINE_GPU_BACKEND}').")
endif()
if(NOT MILO_ENGINE_GPU_BACKEND STREQUAL "off" AND NOT MILO_ENGINE_BUILD_GFX)
    message(FATAL_ERROR
        "MILO_ENGINE_GPU_BACKEND=${MILO_ENGINE_GPU_BACKEND} needs the gfx core: "
        "set MILO_ENGINE_BUILD_GFX=ON (or MILO_ENGINE_GPU_BACKEND=off).")
endif()
option(MILO_ENGINE_BUILD_TESTS  "Build the engine-only convergence test suite"  ${MILO_ENGINE_STANDALONE})
option(MILO_ENGINE_LP64_AUDIT   "Enable LP64 pointer-truncation warnings for auditing"          OFF)
option(MILO_ENGINE_ENABLE_ASAN  "Build the engine with AddressSanitizer"                        OFF)

# JSPI (JavaScript Promise Integration) — non-blocking on-demand file fetches
# via emscripten_sleep(), letting the loading screen render while .milo files
# download. Zero code-size overhead vs the old ASYNCIFY (which instrumented
# every function ~50% bloat). Requires Chrome 137+ / Firefox 139+. Used by
# milo_engine_apply_web_target_options() below; only takes effect when
# MILO_BUILD_WEB=ON. C-side gate is MILO_WEB_ASYNCIFY (the renamed
# successor to DC3's DC3_WEB_ASYNCIFY).
option(MILO_WEB_ASYNC "Use JSPI for non-blocking on-demand file fetches (Chrome 137+)" ON)

# -----------------------------------------------------------------------------
# Consumer / reference context
# -----------------------------------------------------------------------------
if(DEFINED MILO_ENGINE_DECOMP_INCLUDE_DIRS AND NOT "${MILO_ENGINE_DECOMP_INCLUDE_DIRS}" STREQUAL "")
    set(MILO_ENGINE_HAVE_CONTEXT ON)
else()
    set(MILO_ENGINE_HAVE_CONTEXT OFF)
endif()

# -----------------------------------------------------------------------------
# Sanitizers / audit (applied to the engine target only)
# -----------------------------------------------------------------------------
set(MILO_ENGINE_EXTRA_FLAGS)
set(MILO_ENGINE_EXTRA_LINK_FLAGS)
if(MILO_ENGINE_ENABLE_ASAN)
    list(APPEND MILO_ENGINE_EXTRA_FLAGS      -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND MILO_ENGINE_EXTRA_LINK_FLAGS -fsanitize=address)
endif()
if(MILO_ENGINE_LP64_AUDIT)
    list(APPEND MILO_ENGINE_EXTRA_FLAGS -Wpointer-to-int-cast -Wint-to-pointer-cast)
endif()

# -----------------------------------------------------------------------------
# Dependencies — only when compiling the real engine (HAVE_CONTEXT). Each is
# guarded by NOT TARGET so a consuming decomp that already set them up wins.
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

if(MILO_ENGINE_HAVE_CONTEXT AND MILO_ENGINE_BUILD_GFX AND NOT EMSCRIPTEN)
    if(NOT TARGET dawn::webgpu_dawn)
        find_package(Dawn REQUIRED)
    endif()
    if(NOT TARGET glfw)
        find_package(glfw3 REQUIRED)
    endif()

    # FFmpeg (Bink video/audio decode). Optional — gates two platform sources.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT TARGET PkgConfig::FFMPEG)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat libavcodec libswscale libavutil)
    endif()
    set(MILO_ENGINE_HAVE_FFMPEG ${FFMPEG_FOUND})

    # Dear ImGui — debug overlay, built into the engine by default.
    if(NOT TARGET imgui)
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        8314fc3e5a10f7c6b670225065fce1dc8cfd396b)
        FetchContent_MakeAvailable(imgui)
        add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp)
        target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
        target_link_libraries(imgui PUBLIC glfw dawn::webgpu_dawn)
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
    endif()
endif()

# -----------------------------------------------------------------------------
# EMSCRIPTEN sibling for the imgui target. Under EMSCRIPTEN the engine's
# non-EMSCRIPTEN imgui block above is skipped, so this stands up an `imgui`
# CMake target that mirrors the desktop one but links emdawnwebgpu/browser-GL
# instead of Dawn/glfw. ImGuiBackend_Web.cpp inside milo-engine then links
# against this target. Input is handled directly via Emscripten's HTML5 API
# from inside ImGuiBackend_Web.cpp (no imgui_impl_glfw under emscripten).
# -----------------------------------------------------------------------------
if(MILO_ENGINE_HAVE_CONTEXT AND MILO_ENGINE_BUILD_GFX AND MILO_BUILD_WEB AND EMSCRIPTEN)
    if(NOT TARGET imgui)
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        8314fc3e5a10f7c6b670225065fce1dc8cfd396b)
        FetchContent_MakeAvailable(imgui)
        add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp)
        target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
        target_compile_options(imgui PUBLIC "SHELL:--use-port=emdawnwebgpu")
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
    endif()
endif()

# -----------------------------------------------------------------------------
# Engine source lists (mirrors dc3-decomp/native source lists, minus per-decomp
# and not-yet-factored files). Web-only sources gated by MILO_BUILD_WEB.
# -----------------------------------------------------------------------------
if(APPLE)
    set(MILO_ENGINE_METAL_SOURCE src/gfx/MetalSurface.mm)
else()
    set(MILO_ENGINE_METAL_SOURCE "")
endif()

# Tier 1 — rndobj-FREE gfx core. These include only gfx/ headers (+ os/utl); no
# rndobj/ coupling. Enough for a clear-color frame (device init, pipelines,
# headless readback, screenshot, imgui). Gated on MILO_ENGINE_BUILD_GFX, so RB3
# (gfx core ON, backends OFF) compiles exactly this set.
set(MILO_ENGINE_GFX_SOURCES
    src/gfx/GpuDevice.cpp
    ${MILO_ENGINE_METAL_SOURCE}
    src/gfx/Screenshot.cpp
    src/gfx/PipelineManager.cpp
    src/gfx/FrameCapture.cpp
    src/gfx/BloomPass.cpp
    src/gfx/GpuResourceRegistry.cpp
    src/gfx/VideoEncoder.cpp
    src/gfx/mikktspace.c
    src/gfx/ImGuiBackend.cpp
)

# Tier 2 — rndobj-COUPLED gfx. These include rndobj/ headers shaped for DC3
# (DrawRect2D->RndMat, DofPass->RndCam, VertexFormats->RndMesh::Vert,
# ShadowPass->RndCam/Env/Lit/Mat/Mesh, PostProcPass->RndPostProc,
# TextureConvert->RndBitmap). Part of the `dc3` backend flavor, so RB3 (older
# rndobj/) drops them. (RB3 variants of these are deferred — see roadmap Phase 2;
# BandRnd does its own DXT decompress + static vert layout inline for now.)
set(MILO_ENGINE_GFX_RNDOBJ_SOURCES
    src/gfx/DrawRect2D.cpp
    src/gfx/DofPass.cpp
    src/gfx/VertexFormats.cpp
    src/gfx/ShadowPass.cpp
    src/gfx/PostProcPass.cpp
    src/gfx/TextureConvert.cpp
)

set(MILO_ENGINE_AUDIO_SOURCES
    src/audio/AudioDevice.cpp
)

set(MILO_ENGINE_CHAR_SOURCES
    src/char/CharTwistSolver.cpp
)

# GPU-coupled platform backends (the `dc3` flavor). These hard-depend on the
# DC3-shaped WebGPU renderer (WgpuRnd : NgRnd, MeshGpuCache, RndTex_Native,
# MaterialSetup) and on DC3's rndobj/ surface. RB3-Wii cannot compile any of
# these — its 2010-era rndobj/ predates the DC3 shapes these files reference
# (e.g. Part_Wgpu calls RndParticleSys::NumTilesAcross()/NumTilesDown() which RB3
# lacks; Rnd_Wgpu's WgpuRnd derives from NgRnd while RB3's Rnd is older/unrelated).
set(MILO_ENGINE_GPU_PLATFORM_SOURCES
    src/platform/MaterialSetup.cpp
    src/platform/MeshGpuCache.cpp
    src/platform/Mesh_Wgpu.cpp
    src/platform/Part_Wgpu.cpp
    src/platform/RndTex_Native.cpp
    src/platform/Rnd_Wgpu.cpp
    src/platform/Tex_Wgpu.cpp
    src/platform/TransparentQueue.cpp
)

# GPU backend for the `rb3` flavor — BandRnd : Rnd, graduated from rb3/native/.
# Self-contained: it owns its strong defs for the engine's weak-stubbed
# RndMesh::DrawShowing / RndTex::SyncBitmap + VertexFormats::StaticLayout/
# SkinnedLayout (CPU DXT decompress + static vert layout inline), so it needs
# neither MILO_ENGINE_GFX_RNDOBJ_SOURCES nor the dc3 platform backends. Reads
# RB3's matched-fork rndobj via the consumer-injected context.
set(MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3
    src/platform/Rnd_Wgpu_RB3.cpp
    src/platform/RB3MeshCache.cpp    # W1.2: per-mesh GPU upload cache (RB3-only; twin of DC3 MeshGpuCache)
    src/platform/RB3TexSharpen.cpp   # progressive in-session texture sharpen (research/13 T1)
)

# Non-GPU platform sources — kept regardless of MILO_ENGINE_BUILD_GFX (file I/O,
# audio device glue, input, threading, the *_Stub off-path slots, the render
# hook seam). These have no WebGPU / DC3-rndobj coupling.
set(MILO_ENGINE_PLATFORM_SOURCES
    src/platform/AsyncFile_Native.cpp
    src/platform/BinkMovie_Stub.cpp
    src/platform/BoneSetup.cpp
    src/platform/Cache_Stub.cpp
    src/platform/Cam_Native.cpp
    src/platform/CDReader_Native.cpp
    src/platform/ChecksumData_Stub.cpp
    src/platform/DataParser_Native.cpp
    src/platform/File_Native.cpp
    src/platform/FxSendNative.cpp
    src/platform/GameRenderHook.cpp
    src/platform/Joypad_Native.cpp
    src/platform/Keyboard_Native.cpp
    src/platform/Keygen_Stub.cpp
    src/platform/MapFile_Stub.cpp
    src/platform/Memory_Native.cpp
    src/platform/NativeCompatFlags.cpp
    src/platform/Net_Stub.cpp
    src/platform/NetworkSocket_Stub.cpp
    src/platform/SampleInst_Native.cpp
    src/platform/StreamReceiver_Native.cpp
    src/platform/Synth_Stub.cpp
    src/platform/SynthCommon_Stub.cpp
    src/platform/ThreadCall_Native.cpp
    src/platform/VirtualKeyboard_Stub.cpp
    # Deferred — these compile in DC3 today but their matched-fork interface
    # headers drag in SDK/game couplings that need HX_NATIVE-gating before the
    # file is engine-clean (Phase-1 factoring). They stay DC3 glue for now:
    #   PlatformMgr_Native.cpp  os/PlatformMgr.h -> xdk/XSOCIAL.h (Xbox social)
    #   RenderState_Native.cpp  rnddx9/RenderState.h -> xdk/D3D9.h (Xbox D3D9)
    #   Skeleton_Native.cpp     gesture/Skeleton.h -> xdk/NUI.h (Kinect; DC3-only)
    #   HttpServer.cpp          telemetry/GameplayTelemetry.h (DC3 telemetry)
    #   DebugPanel.cpp          references TheHttpServer under DC3_HTTP_SERVER;
    #                           pairs with HttpServer — factor the HTTP/telemetry
    #                           hook, then both move in together.
)
if(MILO_ENGINE_GPU_BACKEND STREQUAL "dc3")
    list(APPEND MILO_ENGINE_PLATFORM_SOURCES ${MILO_ENGINE_GPU_PLATFORM_SOURCES})
elseif(MILO_ENGINE_GPU_BACKEND STREQUAL "rb3")
    list(APPEND MILO_ENGINE_PLATFORM_SOURCES ${MILO_ENGINE_GPU_PLATFORM_SOURCES_RB3})
endif()
if(MILO_ENGINE_HAVE_FFMPEG)
    list(APPEND MILO_ENGINE_PLATFORM_SOURCES
        src/platform/FFmpegAudioReader.cpp
        src/platform/FFmpegMovieImpl.cpp)
endif()

set(MILO_ENGINE_FULL_SOURCES
    ${MILO_ENGINE_AUDIO_SOURCES}
    ${MILO_ENGINE_CHAR_SOURCES}
    ${MILO_ENGINE_PLATFORM_SOURCES}
)
if(MILO_ENGINE_BUILD_GFX)
    list(APPEND MILO_ENGINE_FULL_SOURCES ${MILO_ENGINE_GFX_SOURCES})
endif()
# The 6 rndobj-coupled gfx TUs are part of the dc3 flavor only (their RB3
# variants are deferred — Phase 2).
if(MILO_ENGINE_GPU_BACKEND STREQUAL "dc3")
    list(APPEND MILO_ENGINE_FULL_SOURCES ${MILO_ENGINE_GFX_RNDOBJ_SOURCES})
endif()

# -----------------------------------------------------------------------------
# Web platform layer (Emscripten). Gated on MILO_BUILD_WEB AND EMSCRIPTEN —
# these TUs all `#ifdef __EMSCRIPTEN__` internally too, but the gate keeps
# them out of the engine's source list on non-web builds entirely.
#
# Replaces the corresponding *_Native / desktop TUs:
#   File_Native.cpp     -> File_Web.cpp        (MEMFS-backed POSIX I/O)
#   CDReader_Native.cpp -> CDReader_Web.cpp    (.ark archive system bypass)
#   AudioDevice.cpp     -> AudioDevice_Web.cpp (AudioWorklet+SAB ring buffer)
#   GpuDevice.cpp       -> GpuDevice_Web.cpp   (emdawnwebgpu canvas surface)
#   ImGuiBackend.cpp    -> ImGuiBackend_Web.cpp (HTML5 input + imgui_impl_wgpu)
# plus WebAssets.cpp (Emscripten fetch helper, no desktop equivalent).
# -----------------------------------------------------------------------------
set(MILO_ENGINE_WEB_PLATFORM_SOURCES
    src/platform/WebAssets.cpp
    src/platform/File_Web.cpp
    src/platform/CDReader_Web.cpp
    src/platform/GpuDevice_Web.cpp
    src/audio/AudioDevice_Web.cpp
)
set(MILO_ENGINE_WEB_GFX_SOURCES
    src/gfx/ImGuiBackend_Web.cpp
)
if(MILO_BUILD_WEB AND EMSCRIPTEN)
    # Drop the desktop TUs that the *_Web variants replace.
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/File_Native\\.cpp$")
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/CDReader_Native\\.cpp$")
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/AudioDevice\\.cpp$")
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/GpuDevice\\.cpp$")
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/ImGuiBackend\\.cpp$")

    list(APPEND MILO_ENGINE_FULL_SOURCES ${MILO_ENGINE_WEB_PLATFORM_SOURCES})
    if(MILO_ENGINE_BUILD_GFX)
        list(APPEND MILO_ENGINE_FULL_SOURCES ${MILO_ENGINE_WEB_GFX_SOURCES})
    endif()
endif()

# Consumer-injected extra exclusions. A consuming decomp whose matched-fork
# headers are an OLDER shape than the engine's source set was written against
# can name the engine TUs that don't compile against its headers (each entry is
# a basename, e.g. "Synth_Stub.cpp"). These are dropped from the engine lib for
# that consumer only — the default build is unaffected. Every exclusion must be
# a genuine header-shape mismatch on an off-path subsystem, NOT a workaround for
# a real engine bug (the consumer documents each in its own CMakeLists).
foreach(_excl ${MILO_ENGINE_DECOMP_PLATFORM_EXCLUDE})
    list(FILTER MILO_ENGINE_FULL_SOURCES EXCLUDE REGEX "/${_excl}$")
endforeach()

# -----------------------------------------------------------------------------
# Engine static library
# -----------------------------------------------------------------------------
set(MILO_ENGINE_SOURCES src/EngineVersion.cpp)
if(MILO_ENGINE_HAVE_CONTEXT)
    list(APPEND MILO_ENGINE_SOURCES ${MILO_ENGINE_FULL_SOURCES})
endif()

add_library(milo-engine STATIC ${MILO_ENGINE_SOURCES})
target_compile_features(milo-engine PUBLIC cxx_std_17)

set_source_files_properties(src/gfx/mikktspace.c PROPERTIES LANGUAGE C)

# STL ABI seam: the host-STL shim headers (include/bits, include/__iterator) and
# the STLport-shape shims (src/stl) MUST precede the system + decomp headers so
# host clang STL wins. BEFORE forces them ahead of everything.
target_include_directories(milo-engine BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(milo-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(milo-engine PRIVATE HX_NATIVE=1)
target_compile_options(milo-engine PRIVATE ${MILO_ENGINE_EXTRA_FLAGS})
target_link_options(milo-engine PRIVATE ${MILO_ENGINE_EXTRA_LINK_FLAGS})
target_link_libraries(milo-engine PUBLIC Threads::Threads)

if(MILO_ENGINE_HAVE_CONTEXT)
    # Consumer-injected Milo headers + matched-fork compat flags + defs.
    target_include_directories(milo-engine PUBLIC ${MILO_ENGINE_DECOMP_INCLUDE_DIRS})
    target_compile_options(milo-engine PRIVATE ${MILO_ENGINE_DECOMP_COMPAT_FLAGS})
    # DC3_HTTP_SERVER is intentionally NOT set here — it's a per-decomp debug
    # flag; the consumer defines it on its own target (and compiles
    # HttpServer.cpp + DebugPanel.cpp, which are deferred glue).
    target_compile_definitions(milo-engine PRIVATE
        MILO_DEBUG=1 _DEBUG=1
        ${MILO_ENGINE_DECOMP_DEFINITIONS})
    if(MILO_ENGINE_BUILD_GFX)
        # HX_IMGUI for ImGuiBackend (only built under GFX).
        target_compile_definitions(milo-engine PRIVATE HX_IMGUI=1)
        if(MILO_ENGINE_HAVE_FFMPEG)
            target_compile_definitions(milo-engine PRIVATE HX_FFMPEG=1)
            target_include_directories(milo-engine PUBLIC ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(milo-engine PUBLIC PkgConfig::FFMPEG)
        endif()
        if(NOT EMSCRIPTEN)
            target_link_libraries(milo-engine PUBLIC dawn::webgpu_dawn glfw imgui)
        elseif(MILO_BUILD_WEB AND TARGET imgui)
            # Web build: link the EMSCRIPTEN sibling imgui target (see top of
            # this file). emdawnwebgpu is supplied via --use-port; no Dawn/glfw.
            target_link_libraries(milo-engine PUBLIC imgui)
        endif()
    endif()

    # These TUs conflict with the decomp PCH (see dc3 CMake notes): File_Native
    # (FileStat vs <sys/stat.h> st_*time macros), HttpServer (httplib heavy).
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/File_Native.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    if(MILO_ENGINE_DECOMP_PCH AND NOT "${MILO_ENGINE_DECOMP_PCH}" STREQUAL "")
        target_precompile_headers(milo-engine PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${MILO_ENGINE_DECOMP_PCH}>")
    endif()
endif()

# -----------------------------------------------------------------------------
# Opt-in tool / test / web targets — land in later passes.
# -----------------------------------------------------------------------------
if(MILO_ENGINE_BUILD_TOOLS)
    message(STATUS "milo-engine: tool targets not yet ported (milo-viewer/milo2gltf/render-test).")
endif()
if(MILO_ENGINE_BUILD_TESTS)
    enable_testing()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
        add_subdirectory(tests)
    else()
        message(STATUS "milo-engine: engine-only test suite not yet ported.")
    endif()
endif()

# -----------------------------------------------------------------------------
# Helper: milo_engine_apply_web_target_options(<target> [<asset_dest_dir>])
#
# Applies the canonical Emscripten link/compile flag set the engine's web
# infra needs (emdawnwebgpu, wasm-exceptions, JSPI when MILO_WEB_ASYNC=ON,
# stack/memory sizing, FETCH, ZLIB/OGG/VORBIS, ASSERTIONS, stack-overflow
# check, debug symbols, missing_stubs.js pre-js, base _main export list).
# Consumers extend EXPORTED_FUNCTIONS / JSPI_EXPORTS for their own tick
# symbol by appending another target_link_options block — repeated
# -sEXPORTED_FUNCTIONS=[...] / -sJSPI_EXPORTS=[...] entries to wasm-ld
# replace earlier ones, so the LAST occurrence wins. Example consumer:
#
#   milo_engine_apply_web_target_options(dc3-web)
#   milo_engine_set_web_canvas_selector(dc3-web "#dc3-canvas")
#   target_link_options(dc3-web PRIVATE
#       -sEXPORTED_FUNCTIONS=["_main","_dc3MainLoopTick"]
#       -sJSPI_EXPORTS=["_main","_dc3MainLoopTick","dc3MainLoopTick"])
#   target_compile_definitions(dc3-web PRIVATE MILO_WEB_AUDIO_NS=dc3)
#
# The optional second arg is the directory to copy audio-worklet.js into
# (defaults to $<TARGET_FILE_DIR:tgt>, i.e. next to the .wasm). Pass an
# explicit path to override (DC3's existing build.sh copies it to native/
# web/build, for example).
#
# Asset paths (missing_stubs.js, audio-worklet.js) resolve via
# MILO_ENGINE_SOURCE_DIR (snapshotted at the top of this file). Do NOT use
# CMAKE_CURRENT_SOURCE_DIR here — inside a function() it resolves to the
# CALLER's source dir, not the engine's.
#
# The canvas-selector parameter is intentionally NOT plumbed into this
# helper: CMake refuses to pass macro values containing `#` on the compiler
# command line (it strips or mangles them). milo_engine_set_web_canvas_selector
# below routes the value through a configure_file'd, force-included header
# instead; canvas selection is conceptually independent from the link/compile
# option block this helper owns, so it stays a sibling helper rather than an
# optional arg here.
# -----------------------------------------------------------------------------
function(milo_engine_apply_web_target_options _tgt)
    if(NOT EMSCRIPTEN)
        message(WARNING
            "milo_engine_apply_web_target_options(${_tgt}) called without an "
            "Emscripten toolchain — no-op. Configure with emcmake.")
        return()
    endif()

    set(_asset_dest "${ARGV1}")

    target_compile_options(${_tgt} PRIVATE
        "SHELL:--use-port=emdawnwebgpu"
        -fwasm-exceptions)

    target_link_options(${_tgt} PRIVATE
        "SHELL:--use-port=emdawnwebgpu"
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=512MB
        -sSTACK_SIZE=4194304
        -sENVIRONMENT=web,node
        -sERROR_ON_UNDEFINED_SYMBOLS=0
        -sUSE_ZLIB=1
        -sUSE_OGG=1
        -sUSE_VORBIS=1
        -sFETCH=1
        -sASSERTIONS=1
        -sSTACK_OVERFLOW_CHECK=2
        # Base export set — consumers extend with their own tick symbol(s) via
        # an additional target_link_options(... -sEXPORTED_FUNCTIONS=...) call.
        -sEXPORTED_FUNCTIONS=[\"_main\"]
        -sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\"]
        -fwasm-exceptions
        -g2
        --pre-js ${MILO_ENGINE_SOURCE_DIR}/src/platform/web/assets/missing_stubs.js)

    if(MILO_WEB_ASYNC)
        target_compile_definitions(${_tgt} PRIVATE MILO_WEB_ASYNCIFY=1)
        target_link_options(${_tgt} PRIVATE
            -sJSPI
            -sJSPI_EXPORTS=[\"_main\"])
    endif()

    # Deploy the AudioWorklet JS next to the wasm at build time.
    if(_asset_dest STREQUAL "")
        set(_asset_dest "$<TARGET_FILE_DIR:${_tgt}>")
    endif()
    add_custom_command(TARGET ${_tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MILO_ENGINE_SOURCE_DIR}/src/platform/web/assets/audio-worklet.js
            ${_asset_dest}/audio-worklet.js
        COMMENT "milo-engine: deploying audio-worklet.js next to ${_tgt}")
endfunction()

# -----------------------------------------------------------------------------
# Helper: milo_engine_set_web_canvas_selector(<target> <selector>)
#
# Sets the MILO_WEB_CANVAS_SELECTOR preprocessor macro for <target> to the
# given DOM selector string (e.g. "#dc3-canvas", "#rb3-canvas"). The macro
# value contains `#`, which CMake refuses to pass on a compiler command line
# (target_compile_definitions(... MILO_WEB_CANVAS_SELECTOR="#foo") silently
# drops the macro with a warning; the SHELL:-D…="\#…" workaround gets mangled
# by ninja/make into a compile error). So we route the value through a
# per-target header generated via configure_file() and force-include it into
# every TU compiled for <target>.
#
# Sibling helper rather than an optional arg on milo_engine_apply_web_target_options
# because canvas selection is conceptually independent from the link/compile
# option block — a consumer might want to set the selector but customise the
# link options entirely (or vice versa).
#
# Example:
#   milo_engine_set_web_canvas_selector(dc3-web "#dc3-canvas")
#   milo_engine_set_web_canvas_selector(rb3-web "#rb3-canvas")
# -----------------------------------------------------------------------------
function(milo_engine_set_web_canvas_selector _tgt _selector)
    if(NOT EMSCRIPTEN)
        message(WARNING
            "milo_engine_set_web_canvas_selector(${_tgt}) called without an "
            "Emscripten toolchain — no-op. Configure with emcmake.")
        return()
    endif()

    set(MILO_WEB_CANVAS_SELECTOR "${_selector}")
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/milo-engine-web-config/${_tgt}")
    set(_out_file "${_out_dir}/milo_web_config_${_tgt}.h")
    configure_file(
        "${MILO_ENGINE_SOURCE_DIR}/src/platform/web/milo_web_config.h.in"
        "${_out_file}"
        @ONLY)

    target_include_directories(${_tgt} PRIVATE "${_out_dir}")
    # SHELL: keeps the two tokens together so the generator doesn't split or
    # de-dup them with adjacent -include flags from another helper call.
    target_compile_options(${_tgt} PRIVATE
        "SHELL:-include milo_web_config_${_tgt}.h")
endfunction()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "milo-engine configured:")
message(STATUS "    standalone / context : ${MILO_ENGINE_STANDALONE} / ${MILO_ENGINE_HAVE_CONTEXT}")
message(STATUS "    compiler             : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(MILO_ENGINE_HAVE_CONTEXT)
    list(LENGTH MILO_ENGINE_FULL_SOURCES _n)
    message(STATUS "    engine sources       : ${_n} (+ EngineVersion)")
    message(STATUS "    gfx (WebGPU core)    : ${MILO_ENGINE_BUILD_GFX}")
    message(STATUS "    gpu backend flavor   : ${MILO_ENGINE_GPU_BACKEND}")
    message(STATUS "    FFmpeg               : ${MILO_ENGINE_HAVE_FFMPEG}")
    if(MILO_BUILD_WEB AND EMSCRIPTEN)
        message(STATUS "    web (Emscripten)     : ON (MILO_WEB_ASYNC=${MILO_WEB_ASYNC})")
    endif()
else()
    message(STATUS "    engine sources       : placeholder-only (no decomp context)")
endif()
