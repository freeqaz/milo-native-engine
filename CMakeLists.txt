cmake_minimum_required(VERSION 3.22)

# =============================================================================
# milo-native-engine
#
# Shared LP64 modern-C++ runtime for native ports of Milo-engine games
# (Dance Central 3, Rock Band 3 Wii, Rock Band 3 Xbox 360). The engine is
# GAME-AGNOSTIC and SDK-AGNOSTIC: it never references per-game classes
# (hamobj/, band3/, dance/, meta_*) or per-platform SDK headers (xdk/,
# revolution/). Each decomp consumes the engine via add_subdirectory() and
# assembles its own executable from engine + its matched fork + its native glue.
#
# Canonical plan: rb3/docs/native/NATIVE_PORT_ROADMAP.md
# Disposition catalog: rb3/docs/native/NATIVE_PORT_INVENTORY.md
#
# Phase 0.1 (this commit): the skeleton. milo-engine builds as a valid (near-
# empty) static archive from a single placeholder TU. The subsystem source
# trees (gfx/, audio/, platform/, char/, stl/, system/) are empty and land in
# Phase 0.2. Dependency detection below is SOFT (warn, don't fail) precisely
# because the skeleton has no sources that need those deps yet; Phase 0.2 flips
# the ones it consumes to REQUIRED.
# =============================================================================

# Is the engine the top-level build, or pulled in via add_subdirectory() by a
# decomp? Standalone-only setup (compiler autodetect, default-on tests) is gated
# on this so consuming a decomp build doesn't clobber the parent's choices.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MILO_ENGINE_STANDALONE ON)
else()
    set(MILO_ENGINE_STANDALONE OFF)
endif()

# The native build targets vanilla Clang LP64 C++17. Consumers (the decomps)
# already select Clang for their matched-fork MSVC/MWCC compat flags; when built
# standalone, autodetect clang to match. (Emscripten ships its own Clang.)
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

# Engine .cpp is clean LP64 and would compile under GCC, but we standardize on
# Clang to match the decomp builds (which require it for compat flags). Warn,
# don't fail — a stray GCC configure of the skeleton alone is harmless.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "milo-engine targets Clang LP64 (found ${CMAKE_CXX_COMPILER_ID}). "
        "The decomp builds that consume this engine require Clang; configure "
        "with CC=clang CXX=clang++ to match.")
endif()

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(MILO_ENGINE_BUILD_TOOLS  "Build engine CLI tools (milo-viewer, milo2gltf, render-test)" OFF)
option(MILO_BUILD_WEB           "Build the Emscripten/web target machinery"                     OFF)
option(MILO_ENGINE_BUILD_TESTS  "Build the engine-only convergence test suite"  ${MILO_ENGINE_STANDALONE})
option(MILO_ENGINE_LP64_AUDIT   "Enable LP64 pointer-truncation warnings for auditing"          OFF)
option(MILO_ENGINE_ENABLE_ASAN  "Build the engine with AddressSanitizer"                        OFF)

# -----------------------------------------------------------------------------
# Sanitizers / audit flags (applied to the engine target only, below)
# -----------------------------------------------------------------------------
set(MILO_ENGINE_PRIVATE_FLAGS)
set(MILO_ENGINE_PRIVATE_LINK_FLAGS)
if(MILO_ENGINE_ENABLE_ASAN)
    list(APPEND MILO_ENGINE_PRIVATE_FLAGS      -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND MILO_ENGINE_PRIVATE_LINK_FLAGS -fsanitize=address)
endif()
if(MILO_ENGINE_LP64_AUDIT)
    # cmake -DMILO_ENGINE_LP64_AUDIT=ON ... 2>&1 | grep -E 'pointer-to-int|int-to-pointer'
    list(APPEND MILO_ENGINE_PRIVATE_FLAGS -Wpointer-to-int-cast -Wint-to-pointer-cast)
endif()

# -----------------------------------------------------------------------------
# Dependencies (SOFT in Phase 0.1 — see header comment)
#
# Phase 0.2 consumes: Dawn (WebGPU), glfw3, ImGui (FetchContent), FFmpeg
# (pkg-config: libavformat/libavcodec/libswscale/libavutil), miniaudio
# (vendored in third_party/). For now we probe-and-report so the empty archive
# configures on any machine, and Phase 0.2 has a known detection point to
# promote to REQUIRED.
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

if(EMSCRIPTEN)
    # Emscripten supplies its own threading, WebGPU (emdawnwebgpu) and GLFW shim.
    set(MILO_ENGINE_HAVE_DAWN  OFF)
    set(MILO_ENGINE_HAVE_GLFW  OFF)
    set(MILO_ENGINE_HAVE_FFMPEG OFF)
else()
    find_package(Dawn QUIET)
    find_package(glfw3 QUIET)
    set(MILO_ENGINE_HAVE_DAWN ${Dawn_FOUND})
    set(MILO_ENGINE_HAVE_GLFW ${glfw3_FOUND})

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat libavcodec libswscale libavutil)
    endif()
    set(MILO_ENGINE_HAVE_FFMPEG ${FFMPEG_FOUND})
endif()

# -----------------------------------------------------------------------------
# Engine static library
#
# Phase 0.2 appends globbed/explicit sources from src/gfx, src/audio,
# src/platform, src/char, src/stl, src/system here. For now: the placeholder TU
# only, which is enough for a valid `libmilo-engine.a`.
# -----------------------------------------------------------------------------
set(MILO_ENGINE_SOURCES
    src/EngineVersion.cpp
)

add_library(milo-engine STATIC ${MILO_ENGINE_SOURCES})

target_compile_features(milo-engine PUBLIC cxx_std_17)

# Engine code is always the native build.
target_compile_definitions(milo-engine PRIVATE HX_NATIVE=1)

# PUBLIC so consuming decomp executables inherit the engine's public headers.
target_include_directories(milo-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_options(milo-engine PRIVATE ${MILO_ENGINE_PRIVATE_FLAGS})
target_link_options(milo-engine PRIVATE ${MILO_ENGINE_PRIVATE_LINK_FLAGS})

target_link_libraries(milo-engine PUBLIC Threads::Threads)

# Phase 0.2 will add, conditioned on the HAVE_ flags above:
#   target_link_libraries(milo-engine PUBLIC dawn::dawn glfw imgui ...)
#   if(MILO_ENGINE_HAVE_FFMPEG) target_link_libraries(milo-engine PUBLIC PkgConfig::FFMPEG) endif()

# -----------------------------------------------------------------------------
# Opt-in tool targets (MILO_ENGINE_BUILD_TOOLS=ON) — populated in Phase 0.2.
# -----------------------------------------------------------------------------
if(MILO_ENGINE_BUILD_TOOLS)
    message(STATUS "milo-engine: tool targets requested but not yet ported "
                   "(milo-viewer/milo2gltf/render-test land in Phase 0.2).")
endif()

# -----------------------------------------------------------------------------
# Engine-only convergence test suite — the cross-decomp gate (roadmap §0.2).
# The engine-only test subset moves here from dc3-decomp/native/tests/ in 0.2.
# -----------------------------------------------------------------------------
if(MILO_ENGINE_BUILD_TESTS)
    enable_testing()
    message(STATUS "milo-engine: test suite requested; engine-only tests land in Phase 0.2.")
endif()

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------
message(STATUS "milo-engine configured:")
message(STATUS "    standalone build : ${MILO_ENGINE_STANDALONE}")
message(STATUS "    compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "    tools / web      : ${MILO_ENGINE_BUILD_TOOLS} / ${MILO_BUILD_WEB}")
message(STATUS "    tests            : ${MILO_ENGINE_BUILD_TESTS}")
message(STATUS "    deps  Dawn/glfw  : ${MILO_ENGINE_HAVE_DAWN} / ${MILO_ENGINE_HAVE_GLFW}")
message(STATUS "    deps  FFmpeg     : ${MILO_ENGINE_HAVE_FFMPEG}")
