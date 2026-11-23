# =============================================================================
# DC3 reference-context configure for milo-engine.
#
# Compiles the real engine against dc3-decomp's Milo headers + MSVC-PPC matched-
# fork compat flags. This is the engine's own compile-check / convergence build
# (no full dc3-native relink needed). Usage from the engine repo root:
#
#     cmake -B build-dc3ref -C cmake/dc3-reference.cmake
#     cmake --build build-dc3ref
#
# It reproduces the include paths + compat flags dc3-decomp/native/CMakeLists.txt
# applies to its engine sources. Keep in sync if DC3's flags change.
# =============================================================================

# Where dc3-decomp is: $ENV{DC3_ROOT} if set, else the engine checkout's sibling,
# else (an engine WORKTREE, whose own siblings are unrelated) the sibling of the
# engine's MAIN checkout, found through `git rev-parse --git-common-dir`.
# dc3-decomp-deps (Dawn) is looked up next to whichever dc3 was chosen.
if(DEFINED ENV{DC3_ROOT} AND NOT "$ENV{DC3_ROOT}" STREQUAL "")
    get_filename_component(_DC3 "$ENV{DC3_ROOT}" ABSOLUTE)
else()
    get_filename_component(_DC3 "${CMAKE_CURRENT_LIST_DIR}/../../dc3-decomp" ABSOLUTE)
    if(NOT EXISTS "${_DC3}/src/system/utl/Symbol.cpp")
        execute_process(
            COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --path-format=absolute --git-common-dir
            OUTPUT_VARIABLE _ENGINE_GIT_COMMON OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _rc ERROR_QUIET)
        if(_rc EQUAL 0)
            get_filename_component(_ENGINE_MAIN "${_ENGINE_GIT_COMMON}" DIRECTORY)
            get_filename_component(_DC3 "${_ENGINE_MAIN}/../dc3-decomp" ABSOLUTE)
        endif()
    endif()
endif()
if(NOT EXISTS "${_DC3}/src/system/utl/Symbol.cpp")
    message(FATAL_ERROR "dc3-reference.cmake: no dc3-decomp checkout at ${_DC3}; set DC3_ROOT in the environment.")
endif()
set(MILO_ENGINE_DC3_ROOT "${_DC3}" CACHE PATH "dc3-decomp checkout this reference build reads")

# Dawn (WebGPU) prebuilt — same location dc3-decomp uses.
get_filename_component(_DAWN_SIB "${_DC3}/../dc3-decomp-deps/dawn" ABSOLUTE)
if(NOT EXISTS "${_DAWN_SIB}")
    # a dc3 worktree's siblings are unrelated too: use the main checkout's
    execute_process(
        COMMAND git -C "${_DC3}" rev-parse --path-format=absolute --git-common-dir
        OUTPUT_VARIABLE _DC3_GIT_COMMON OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    get_filename_component(_DC3_MAIN "${_DC3_GIT_COMMON}" DIRECTORY)
    get_filename_component(_DAWN_SIB "${_DC3_MAIN}/../dc3-decomp-deps/dawn" ABSOLUTE)
endif()
set(_DAWN "${_DAWN_SIB}")
set(CMAKE_PREFIX_PATH "${_DAWN}" CACHE STRING "")

# Decomp Milo header roots (mirrors dc3-native target_include_directories).
set(MILO_ENGINE_DECOMP_INCLUDE_DIRS
    "${_DC3}/src"
    "${_DC3}/src/system"
    "${_DC3}/src/system/oggvorbis"
    "${_DC3}/src/lazer"
    "${_DC3}/include"
    CACHE STRING "")

# MSVC-PPC matched-fork compat flags + atomic builtins + decomp flags.
# Mirrors DC3_MSVC_COMPAT_FLAGS + DC3_ATOMIC_COMPAT_FLAGS + DC3_DECOMP_FLAGS.
set(MILO_ENGINE_DECOMP_COMPAT_FLAGS
    -fms-extensions
    -fms-compatibility
    -fms-compatibility-version=19.29
    -fdelayed-template-parsing
    -D__GNUC_STDC_INLINE__
    -D__GCC_ATOMIC_TEST_AND_SET_TRUEVAL=1
    # dc3's DC3_MSVC_COMPAT_FLAGS carries this (2026-08): without it MS-compat TUs
    # call an out-of-line std::type_info::operator== libstdc++ does not export.
    -D__GXX_TYPEINFO_EQUALITY_INLINE=1
    -D__GCC_ATOMIC_BOOL_LOCK_FREE=2
    -D__GCC_ATOMIC_CHAR_LOCK_FREE=2
    -D__GCC_ATOMIC_WCHAR_T_LOCK_FREE=2
    -D__GCC_ATOMIC_CHAR16_T_LOCK_FREE=2
    -D__GCC_ATOMIC_CHAR32_T_LOCK_FREE=2
    -D__GCC_ATOMIC_SHORT_LOCK_FREE=2
    -D__GCC_ATOMIC_INT_LOCK_FREE=2
    -D__GCC_ATOMIC_LONG_LOCK_FREE=2
    -D__GCC_ATOMIC_LLONG_LOCK_FREE=2
    -D__GCC_ATOMIC_POINTER_LOCK_FREE=2
    -ferror-limit=0
    -fno-omit-frame-pointer
    -w
    "SHELL:-include ${_DC3}/native/src/msvc_compat.h"
    CACHE STRING "")

# Reuse the decomp PCH (Object.h/Debug.h + heavy transitive includes).
set(MILO_ENGINE_DECOMP_PCH "${_DC3}/src/system/decomp_pch.h" CACHE FILEPATH "")
