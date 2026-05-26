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

get_filename_component(_DC3 "${CMAKE_CURRENT_LIST_DIR}/../../dc3-decomp" ABSOLUTE)

# Dawn (WebGPU) prebuilt — same location dc3-decomp uses.
get_filename_component(_DAWN "${CMAKE_CURRENT_LIST_DIR}/../../dc3-decomp-deps/dawn" ABSOLUTE)
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
