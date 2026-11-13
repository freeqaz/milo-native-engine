# MiloHostStlShim.cmake -- generate the MSVC-STL iterator shim from THIS toolchain.
#
# Why a shim exists at all
# ------------------------
# Xbox 360 MSVC's STL used a raw `T*` as vector::iterator, and ~600 DC3 decomp call
# sites rely on that (`T** it = vec.begin()`, passing an iterator where a
# pointer is expected). Modern standard libraries wrap the pointer in a class
# (`__gnu_cxx::__normal_iterator` in libstdc++, `std::__wrap_iter` in libc++).
# The shim is the host library's own header with ONE added member: an implicit
# conversion back to the wrapped pointer.
#
# Why it is generated, not checked in
# -----------------------------------
# It used to be a checked-in copy of one library version's header
# (libstdc++ from GCC 15.2.1 and libc++ from Emscripten 5.0.2). A private STL
# header is only valid against the exact library that ships it:
#
#   * GitHub's ubuntu-24.04 runner has GCC 14. GCC 15 moved __niter_base /
#     __niter_wrap from stl_algobase.h into stl_iterator.h, so the GCC-15 copy
#     redefined them and every TU failed ("redefinition of '__niter_base'").
#     That is why dc3-decomp's native-build workflow was red in 36 of 40 runs (H-3).
#   * Against libc++ 23 the Emscripten-5.0.2 copy fails 867 of 874 TUs (M-1),
#     and Apple's libc++ is a third version again.
#
# So at configure time we ask the compiler which header it actually uses (a
# `-M` dependency scan of <vector>, with the same CMAKE_CXX_FLAGS the build
# uses, so -stdlib=libc++ / --sysroot / em++ are honoured), copy that header,
# and insert the one operator after an anchor. The anchor must match EXACTLY
# once or configure fails -- a silently unpatched shim would compile the 600
# call sites into errors, and a doubly patched one into ambiguity.
#
# Usage:
#   include(<engine>/cmake/MiloHostStlShim.cmake)
#   milo_generate_host_stl_shim(<out_dir> <out_var>)
#   include_directories(BEFORE ${<out_var>})   # must precede every other -I
#
# The output is rewritten only when its content changes, so reconfiguring does
# not invalidate the PCH. The source header is a configure dependency, so a
# toolchain upgrade that replaces it regenerates the shim.

function(milo_generate_host_stl_shim out_dir out_var)
    set(_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/host_stl_shim_probe")
    file(MAKE_DIRECTORY "${_probe_dir}")
    set(_probe_src "${_probe_dir}/probe.cpp")
    file(WRITE "${_probe_src}" "#include <vector>\n#include <string>\n")

    # Same flags the real compile uses for stdlib selection. CMAKE_CXX_FLAGS
    # carries -stdlib=libc++ for the Linux libc++ proxy build; the target /
    # sysroot cover cross toolchains. Emscripten's em++ supplies its sysroot.
    separate_arguments(_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
    if(CMAKE_CXX_COMPILER_TARGET)
        list(APPEND _flags "--target=${CMAKE_CXX_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
        list(APPEND _flags "--sysroot=${CMAKE_SYSROOT}")
    endif()
    if(CMAKE_OSX_SYSROOT AND APPLE)
        list(APPEND _flags -isysroot "${CMAKE_OSX_SYSROOT}")
    endif()

    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" ${_flags} -std=gnu++17 -x c++ -M "${_probe_src}"
        WORKING_DIRECTORY "${_probe_dir}"
        OUTPUT_VARIABLE _deps
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "HostStlShim: dependency scan of <vector> failed "
                            "(${CMAKE_CXX_COMPILER} -M, rc=${_rc}):\n${_err}")
    endif()

    # -M output: "probe.o: probe.cpp /path/a.h \\\n /path/b.h ..." -> one path
    # per list element.
    string(REPLACE "\\\n" " " _deps "${_deps}")
    string(REGEX REPLACE "^[^:]*:" "" _deps "${_deps}")
    separate_arguments(_deps UNIX_COMMAND "${_deps}")

    set(_kind "")
    set(_src "")
    foreach(_h IN LISTS _deps)
        if(_h MATCHES "/bits/stl_iterator\\.h$")
            set(_kind libstdcxx)
            set(_src "${_h}")
            set(_rel bits/stl_iterator.h)
            break()
        elseif(_h MATCHES "/__iterator/wrap_iter\\.h$")
            set(_kind libcxx)
            set(_src "${_h}")
            set(_rel __iterator/wrap_iter.h)
            break()
        endif()
    endforeach()
    if(_kind STREQUAL "")
        message(FATAL_ERROR "HostStlShim: <vector> pulled in neither "
            "bits/stl_iterator.h (libstdc++) nor __iterator/wrap_iter.h (libc++). "
            "Unknown standard library; the MSVC iterator shim cannot be generated.")
    endif()
    get_filename_component(_src "${_src}" REALPATH)
    file(READ "${_src}" _text)

    # One anchor per library, each the start of the class's public section, so
    # the inserted member is public and is not inside any #if branch. The
    # whitespace classes tolerate both the old Allman layout (Apple libc++,
    # libc++ <= 17) and the current clang-format layout.
    if(_kind STREQUAL "libstdcxx")
        # libstdc++ has used this exact accessor since at least GCC 10.
        set(_anchor_re "const _Iterator&[ \t\r\n]+base\\(\\) const _GLIBCXX_NOEXCEPT[ \t\r\n]+{ return _M_current; }")
        set(_insert "\n\n      // Milo native engine (generated by milo-native-engine/cmake/MiloHostStlShim.cmake):\n      // implicit conversion to the wrapped pointer, as with Xbox 360 MSVC's\n      // STL, where vector::iterator is T*.\n      __attribute__((__always_inline__))\n      _GLIBCXX_CONSTEXPR\n      operator _Iterator() const _GLIBCXX_NOEXCEPT\n      { return _M_current; }")
    else()
        set(_anchor_re "class __wrap_iter[ \t\r\n]*{[ \t\r\n]*public:")
        set(_insert "\n  // Milo native engine (generated by milo-native-engine/cmake/MiloHostStlShim.cmake):\n  // implicit conversion to the wrapped pointer, as with Xbox 360 MSVC's\n  // STL, where vector::iterator is T*.\n  _LIBCPP_HIDE_FROM_ABI _LIBCPP_CONSTEXPR_SINCE_CXX14 operator _Iter() const _NOEXCEPT { return __i_; }\n")
    endif()

    # Count by MATCH + "no further MATCH in the remainder", not MATCHALL: the
    # libstdc++ anchor contains ';', and MATCHALL returns a CMake list, so one
    # hit would read as two.
    string(REGEX MATCH "${_anchor_re}" _hit "${_text}")
    set(_n 0)
    if(NOT _hit STREQUAL "")
        set(_n 1)
        string(FIND "${_text}" "${_hit}" _pos)
        string(LENGTH "${_hit}" _alen)
        math(EXPR _cut "${_pos} + ${_alen}")
        string(SUBSTRING "${_text}" 0 ${_cut} _head)
        string(SUBSTRING "${_text}" ${_cut} -1 _tail)
        string(REGEX MATCH "${_anchor_re}" _again "${_tail}")
        if(NOT _again STREQUAL "")
            set(_n "more than 1")
        endif()
    endif()
    if(NOT _n STREQUAL "1")
        message(FATAL_ERROR "HostStlShim: expected exactly 1 anchor in ${_src}, "
            "found ${_n}. The ${_kind} header layout changed; update the anchor "
            "in milo-native-engine/cmake/MiloHostStlShim.cmake (do not check a copied header in).")
    endif()
    set(_out "// GENERATED by milo-native-engine/cmake/MiloHostStlShim.cmake from\n//   ${_src}\n// Do not edit; reconfigure instead.\n${_head}${_insert}${_tail}")

    set(_dst "${out_dir}/${_rel}")
    set(_tmp "${_probe_dir}/shim.tmp")
    file(WRITE "${_tmp}" "${_out}")
    # Only rewrite when the content differs: every TU and the PCH depend on it.
    configure_file("${_tmp}" "${_dst}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")

    message(STATUS "HostStlShim: ${_kind} ${_rel} <- ${_src}")
    set(${out_var} "${out_dir}" PARENT_SCOPE)
    set(${out_var}_KIND "${_kind}" PARENT_SCOPE)
    set(${out_var}_SOURCE "${_src}" PARENT_SCOPE)
endfunction()
