# =============================================================================
# milo-engine-tests — engine-only convergence test suite (cross-decomp gate)
#
# This is the game-agnostic subset of dc3-decomp/native/tests that the roadmap
# (rb3/docs/native/NATIVE_PORT_ROADMAP.md §0.2) designates as the convergence
# gate: the tests that exercise the shared engine runtime (binstream, dirloader,
# dta, mesh/asset loading, charbones, mogg, bink-extract, rndcam, object
# lifetime) rather than any per-game class.
#
# LINK MODEL (read this before changing the source list):
# ------------------------------------------------------------------------------
# The end-state goal is for these tests to link libmilo-engine.a ONLY. That is
# not yet possible: Phase 0.2 has moved only the gfx/audio/platform/char half of
# the runtime into the engine. The Milo CORE runtime that these tests need —
# utl/ (Symbol, BinStream, DataArray), obj/ (Object, Dir, DirLoader), math/,
# rndobj/, os/ (System*), synth/, char/, plus the per-decomp native glue
# (System_Native, Memory_Native, xdk_shims, engine_stubs_generated, ...) that
# test_helpers.cpp's EnsureEngineInit() drives — still lives in the dc3-decomp
# matched fork. test_helpers.cpp::EnsureEngineInit() itself calls HamInit /
# FlowInit / WorldInit / SynthPreInit and registers UI/synth object factories,
# so the "engine-only" fixture is, in practice, still game-coupled today.
#
# So this target links libmilo-engine.a PLUS the exact set of dc3-decomp .cpp
# files that dc3-decomp/native's own `milo-tests` target compiles (see
# dc3_runtime_sources.cmake — generated, read-only consumption of the dc3
# checkout; nothing in dc3-decomp is modified). As Phase 0.2 factors the core
# runtime into the engine, DC3_RUNTIME_SOURCES shrinks toward empty and the
# tests converge on linking the engine alone.
#
# Test sources compile against the reference decomp HEADERS + matched-fork compat
# flags + decomp PCH that the parent CMakeLists exposes as the consumer context
# (MILO_ENGINE_DECOMP_INCLUDE_DIRS / _COMPAT_FLAGS / _PCH), identically to the
# engine .cpp.
# =============================================================================

find_package(GTest REQUIRED)
include(GoogleTest)

# -----------------------------------------------------------------------------
# Locate the dc3-decomp checkout that supplies the not-yet-factored core runtime
# AND the test assets. Defaults to the sibling repo next to milo-native-engine.
# -----------------------------------------------------------------------------
get_filename_component(_DEFAULT_DC3 "${CMAKE_SOURCE_DIR}/../dc3-decomp" ABSOLUTE)
set(DC3_RUNTIME_ROOT "${_DEFAULT_DC3}" CACHE PATH
    "dc3-decomp checkout: supplies the Milo core runtime + native glue + test assets")

if(NOT EXISTS "${DC3_RUNTIME_ROOT}/src/system/utl/Symbol.cpp")
    message(WARNING
        "milo-engine-tests: DC3_RUNTIME_ROOT=${DC3_RUNTIME_ROOT} does not look "
        "like a dc3-decomp checkout (src/system/utl/Symbol.cpp missing). The "
        "tests cannot link until the core Milo runtime is either factored into "
        "the engine or sourced from dc3-decomp. Skipping milo-engine-tests.")
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/dc3_runtime_sources.cmake)

# -----------------------------------------------------------------------------
# Engine-only test subset (roadmap §0.2).
#
# test_asset_loading.cpp is included even though it #includes hamobj/MoveGraph.h
# and flow/Flow.h — those couplings are tolerated for now because the file
# exercises the most critical end-to-end asset load paths in the engine and
# they're the same paths the §0.2 gate is trying to guard. The flaky teardown
# segfault that previously caused this file to be DROPPED was diagnosed (Phase
# 0.2): it was not a GpuDevice/Dawn static-destruction issue at all — it was a
# nullptr deref of `TheUI` inside `PanelDir::SendTransition` reached when a
# HamDirector merge callback fires from a song milo load. dc3's milo-tests
# happens to run fewer setup steps before reaching this load and short-circuits
# earlier; the engine build reaches the callback chain more reliably and hits
# the deref ~70% of runs. Fixed in test_helpers.cpp by installing a default-
# constructed UIManager stub (`TheUI = &sTestUIManager`) before any asset load
# runs. UIManager::Init() is intentionally NOT called — only the read-only
# accessors used during load callbacks (WentBack(), CurrentScreen()) matter,
# and they return safe zero/null defaults on the stub instance.
# -----------------------------------------------------------------------------
set(MILO_ENGINE_TEST_SOURCES
    test_helpers.cpp
    test_binstream.cpp
    test_chunkstream.cpp
    test_dirloader.cpp
    test_dta_parser.cpp
    test_mesh_loading.cpp
    test_archive_enumeration.cpp
    test_asset_loading.cpp
    test_charbones_serialization.cpp
    test_charclipgroup.cpp
    test_bone_ground_truth.cpp
    test_object_lifetime.cpp
    test_rndcam_projection.cpp
    test_mogg_decode.cpp
    test_mogg_v0xe.cpp
    test_extract_bik.cpp
)

# FFmpeg (Bink) — the parent scope detected it as MILO_ENGINE_HAVE_FFMPEG; reuse.
add_executable(milo-engine-tests
    ${MILO_ENGINE_TEST_SOURCES}
    ${DC3_RUNTIME_SOURCES}
)

# Consumer context: reference decomp headers + matched-fork compat flags, exactly
# as the engine .cpp are compiled (so test/runtime TUs share one ABI + STL shape).
target_compile_options(milo-engine-tests PRIVATE ${MILO_ENGINE_DECOMP_COMPAT_FLAGS})
target_compile_definitions(milo-engine-tests PRIVATE
    HX_NATIVE=1 MILO_DEBUG=1 _DEBUG=1
    ${MILO_ENGINE_DECOMP_DEFINITIONS})
if(MILO_ENGINE_HAVE_FFMPEG)
    target_compile_definitions(milo-engine-tests PRIVATE HX_FFMPEG=1)
endif()

# Include dirs: the engine's shim layer first (STL ABI seam — host clang STL must
# win, BEFORE everything), then the reference decomp Milo header roots, then the
# dc3 native-glue header roots that the dc3 runtime .cpp need (xdk shim headers,
# native settings, httplib/stb/cgltf), then the tests dir for test_helpers.h.
target_include_directories(milo-engine-tests BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(milo-engine-tests PRIVATE
    ${MILO_ENGINE_DECOMP_INCLUDE_DIRS}
    ${DC3_RUNTIME_ROOT}/native/src
    ${DC3_RUNTIME_ROOT}/native/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS})

# Decomp PCH (Object.h/Debug.h + heavy transitive includes), as the engine uses.
if(MILO_ENGINE_DECOMP_PCH AND NOT "${MILO_ENGINE_DECOMP_PCH}" STREQUAL "")
    target_precompile_headers(milo-engine-tests PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${MILO_ENGINE_DECOMP_PCH}>")
endif()
# These TUs conflict with the decomp PCH (same carve-outs dc3's milo-tests makes):
#   Jpeg.cpp                 jpeg jmorecfg.h INT32 (long) vs xdk win_types INT32 (int)
#   HttpServer.cpp           httplib heavy / its own includes
#   engine_stubs_generated   generated glue compiled without the PCH context
# (The bundled C sources — oggvorbis/*.c, tomcrypt/*.c, obj/DataFlex.c — are
#  auto-excluded because the PCH genex is CXX-only.)
set_source_files_properties(
    ${DC3_RUNTIME_ROOT}/src/system/jpeg/Jpeg.cpp
    ${DC3_RUNTIME_ROOT}/native/src/platform/HttpServer.cpp
    ${DC3_RUNTIME_ROOT}/native/src/engine_stubs_generated.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Link: the engine static lib + GTest + the platform libs the engine pulls in.
# --allow-multiple-definition / --unresolved-symbols=ignore-all mirror dc3's
# milo-tests link (the engine and the dc3 glue both carry FFmpeg*.cpp + a handful
# of stub/real symbol pairs; the decomp intentionally relies on first-wins).
target_link_libraries(milo-engine-tests PRIVATE
    milo-engine
    GTest::gtest GTest::gtest_main)
if(NOT EMSCRIPTEN)
    if(NOT APPLE)
        target_link_options(milo-engine-tests PRIVATE
            -rdynamic
            -Wl,--unresolved-symbols=ignore-all
            -Wl,--warn-unresolved-symbols
            -Wl,--allow-multiple-definition)
        target_link_libraries(milo-engine-tests PRIVATE dl curl jpeg vorbis vorbisfile ogg z)
    else()
        target_link_options(milo-engine-tests PRIVATE
            -Wl,-ld_classic
            -Wl,-undefined,dynamic_lookup
            -Wl,-multiply_defined,suppress)
        target_link_libraries(milo-engine-tests PRIVATE curl jpeg vorbis vorbisfile ogg z)
    endif()
    if(MILO_ENGINE_HAVE_FFMPEG)
        target_link_libraries(milo-engine-tests PRIVATE PkgConfig::FFMPEG)
    endif()
endif()

# -----------------------------------------------------------------------------
# Test assets. EnsureEngineInit() boots the archive: NativeDetectDataDir() needs
# the game data dir holding gen/main_xbox.hdr (dc3-decomp/orig-assets), found via
# DC3_DATA. The individual tests then locate fixtures via:
#   DC3_DATA        -> game data root (gen/, archive)         => <dc3>/orig-assets
#   MILO_LIB        -> extracted .milo_xbox library           => <dc3>/orig-assets/extracted
#   DC3_REPO_ROOT   -> dc3-decomp root (mogg/bik fixtures)    => <dc3>
#   DC3_AUDIO_TESTS -> opt-in gate for AudioDevice-init tests (left unset)
# Tests GTEST_SKIP() cleanly when an asset/env is absent. We point all of them at
# the dc3 checkout by default so the suite runs without extra setup; override the
# cache vars to relocate. (Asset path approach is documented in the report.)
# -----------------------------------------------------------------------------
set(MILO_TEST_DATA_DIR "${DC3_RUNTIME_ROOT}/orig-assets" CACHE PATH
    "Game data dir with gen/main_xbox.hdr for milo-engine-tests (sets DC3_DATA)")
set(MILO_TEST_MILO_LIB "${DC3_RUNTIME_ROOT}/orig-assets/extracted" CACHE PATH
    "Extracted .milo_xbox asset library for milo-engine-tests (sets MILO_LIB)")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")

# Each test process initializes Dawn/WebGPU + audio (miniaudio). Running many in
# parallel exhausts GPU device handles / fds and causes spurious SEGVs, so the
# whole suite is serialized via a shared RESOURCE_LOCK (run ctest with -j1).
#
# The ENVIRONMENT property is a single semicolon-list of VAR=value entries. The
# separators are escaped (\;) so CMake keeps the whole thing as ONE argument to
# set_tests_properties — otherwise gtest_discover_tests serializes each entry as
# a separate argument and CTest silently keeps only the first (CTest 4.x quirk).
# DC3_AUDIO_TESTS=1 enables the mogg-decode tests. dc3 gates them off by default
# to avoid audio-device contention under parallel ctest; here the whole suite is
# serialized (-j1 via RESOURCE_LOCK) and AudioDevice is headless-skipped, so the
# mogg decode path runs cleanly and is part of the gate.
set(_MILO_TEST_ENV
    "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp\;DC3_DATA=${MILO_TEST_DATA_DIR}\;MILO_LIB=${MILO_TEST_MILO_LIB}\;DC3_REPO_ROOT=${DC3_RUNTIME_ROOT}\;DC3_AUDIO_TESTS=1")
gtest_discover_tests(milo-engine-tests
    PROPERTIES
        RESOURCE_LOCK milo_engine_runtime
        ENVIRONMENT "${_MILO_TEST_ENV}"
)
