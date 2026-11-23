#!/usr/bin/env python3
"""Generate tests/dc3_runtime_sources.cmake from dc3-decomp's REAL milo-tests target.

milo-engine-tests links libmilo-engine.a plus the dc3-decomp runtime that has
not been factored into the engine yet. That runtime used to be a hand-copied
list ("generated" in name only), and it rotted: renamed and deleted dc3 files
broke the configure, and newly added dc3 files went missing, which the old
`--unresolved-symbols=ignore-all` link hid until a test died in a lazy PLT
lookup.

This script derives the list from the configured dc3 build instead, through
CMake's file API (codemodel-v2), so the source set, the per-source carve-outs
(no PCH, C++ for .c, -fno-ms-*), the compile context and the dc3-owned static
libraries are exactly what dc3's own `milo-tests` compiles. Only the test .cpp
(dc3's native/tests/) and the PCH stubs are left out; the engine's own test
files stand in for them.

Usage (from the engine repo root):

    python3 tests/gen_dc3_runtime_sources.py --build-dir <dc3>/native/build

<dc3>/native/build must be a configured dc3 native build (dc3's
scripts/native_configure.sh makes one). The script drops a codemodel-v2 query
into it and re-runs `cmake <build-dir>` (a reconfigure, no build) if there is no
reply yet. Paths are written relative to ${DC3_RUNTIME_ROOT}, so the output does
not depend on which dc3 checkout was read.
"""

import argparse
import glob
import json
import os
import subprocess
import sys

TARGET = "milo-tests"
ENGINE_TARGET = "milo-engine"

# Fragments that describe the build type / PCH mechanics rather than the dc3
# compile context: the consuming build supplies its own.
def _is_mechanics(frag):
    f = frag.strip()
    if f in ("-Xclang", "-fno-pch-timestamp", "-Winvalid-pch"):
        return True
    if "-include-pch" in f or "-emit-pch" in f or "cmake_pch" in f:
        return True
    if f.startswith(("-O", "-g", "-DNDEBUG", "-std=")):
        # "-O2 -g -DNDEBUG -std=gnu++17": build type + language standard
        return all(t.startswith(("-O", "-g", "-DNDEBUG", "-std=")) for t in f.split())
    return False


def _fragments(cg):
    out = []
    for f in cg.get("compileCommandFragments", []):
        frag = f["fragment"]
        if _is_mechanics(frag):
            continue
        out.append(frag)
    return out


def _load_reply(build_dir):
    reply = os.path.join(build_dir, ".cmake", "api", "v1", "reply")
    idx = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not idx:
        return None, reply
    return json.load(open(idx[-1])), reply


def _ensure_reply(build_dir):
    q = os.path.join(build_dir, ".cmake", "api", "v1", "query")
    os.makedirs(q, exist_ok=True)
    qf = os.path.join(q, "codemodel-v2")
    if not os.path.exists(qf):
        open(qf, "w").close()
    index, reply = _load_reply(build_dir)
    cache = os.path.join(build_dir, "CMakeCache.txt")
    stale = index is None or os.path.getmtime(cache) > os.path.getmtime(
        sorted(glob.glob(os.path.join(reply, "index-*.json")))[-1])
    if stale:
        print(f"gen: reconfiguring {build_dir} to get a codemodel reply", file=sys.stderr)
        subprocess.run(["cmake", build_dir], check=True, stdout=subprocess.DEVNULL)
        index, reply = _load_reply(build_dir)
    if index is None:
        sys.exit(f"gen: no CMake file API reply in {reply}")
    return index, reply


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, help="configured dc3-decomp native build dir")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "dc3_runtime_sources.cmake"))
    args = ap.parse_args()
    build_dir = os.path.abspath(args.build_dir)

    index, reply = _ensure_reply(build_dir)
    cm_file = index["reply"]["codemodel-v2"]["jsonFile"]
    cm = json.load(open(os.path.join(reply, cm_file)))
    native_src = os.path.realpath(cm["paths"]["source"])        # <dc3>/native
    dc3_root = os.path.dirname(native_src)
    conf = cm["configurations"][0]
    by_id = {}
    for t in conf["targets"]:
        by_id[t["id"]] = json.load(open(os.path.join(reply, t["jsonFile"])))
    tests = [t for t in by_id.values() if t["name"] == TARGET]
    if not tests:
        sys.exit(f"gen: target {TARGET} not in {build_dir} (BUILD_TESTS off?)")
    tgt = tests[0]

    def rel(path):
        """A source path as ${DC3_RUNTIME_ROOT}/..., or None if outside dc3."""
        p = path if os.path.isabs(path) else os.path.join(native_src, path)
        p = os.path.realpath(p)
        if not (p == dc3_root or p.startswith(dc3_root + os.sep)):
            return None
        return "${DC3_RUNTIME_ROOT}/" + os.path.relpath(p, dc3_root)

    def is_test_or_mechanics(path):
        r = os.path.relpath(os.path.realpath(path if os.path.isabs(path) else os.path.join(native_src, path)),
                            native_src)
        return r.startswith("tests" + os.sep) or r.startswith("build" + os.sep)

    groups = tgt["compileGroups"]
    main_gi = max(range(len(groups)), key=lambda i: len(groups[i]["sourceIndexes"]))
    main = groups[main_gi]
    main_frags = _fragments(main)
    main_defs = [d["define"] for d in main.get("defines", [])]

    runtime = []          # main group
    props = []            # (srcs, properties)
    outside = []
    for gi, cg in enumerate(groups):
        srcs = []
        for si in cg["sourceIndexes"]:
            s = tgt["sources"][si]
            if is_test_or_mechanics(s["path"]):
                continue
            r = rel(s["path"])
            if r is None:
                outside.append(s["path"])
                continue
            srcs.append(r)
        if not srcs:
            continue
        runtime.extend(srcs)
        if gi == main_gi:
            continue
        p = []
        # A SKIP_PRECOMPILE_HEADERS source keeps its group's precompileHeaders
        # entry; what it loses is the -include-pch fragment.
        if not any("-include-pch" in f["fragment"] for f in cg.get("compileCommandFragments", [])):
            p.append(("SKIP_PRECOMPILE_HEADERS", "ON"))
        extra = [f for f in _fragments(cg) if f not in main_frags]
        if extra:
            p.append(("COMPILE_OPTIONS", ";".join(" ".join(extra).split())))
        defs = [d["define"] for d in cg.get("defines", []) if d["define"] not in main_defs]
        if defs:
            p.append(("COMPILE_DEFINITIONS", ";".join(defs)))
        lang = cg["language"]
        default = {".c": "C"}.get(os.path.splitext(srcs[0])[1], "CXX")
        if lang != default:
            p.append(("LANGUAGE", lang))
        if p:
            props.append((srcs, p))
    if outside:
        sys.exit("gen: milo-tests compiles sources outside the dc3 tree, which the engine "
                 "suite cannot name portably:\n  " + "\n  ".join(outside))

    # Compile context of the main group: options (with the dc3 path made relative)
    # and the dc3-owned include roots. Engine / imgui / Dawn includes and the
    # defines those targets export reach the test target through its links.
    opts = []
    for f in main_frags:
        for tok in f.split():
            if tok.startswith(dc3_root + os.sep):
                tok = "${DC3_RUNTIME_ROOT}/" + os.path.relpath(tok, dc3_root)
            opts.append(tok)
    # keep "-include <file>" pairs as one SHELL: item so CMake does not dedupe them
    joined = []
    i = 0
    while i < len(opts):
        if opts[i] == "-include" and i + 1 < len(opts):
            joined.append(f"SHELL:-include {opts[i + 1]}")
            i += 2
        else:
            joined.append(opts[i])
            i += 1
    incs, sys_incs = [], []
    for inc in main.get("includes", []):
        p = os.path.realpath(inc["path"])
        r = rel(p)
        if r is None:
            continue
        rr = os.path.relpath(p, native_src)
        if rr.startswith("build") or rr.startswith("tests"):
            continue
        (sys_incs if inc.get("isSystem") else incs).append(r)
    runtime_defs = [d for d in main_defs
                    if not d.startswith(("GTEST_", "IMGUI_", "MILO_WGPU_CALL_COUNT", "HX_FFMPEG"))]
    # (HX_FFMPEG follows the engine build's own FFmpeg detection in tests/CMakeLists.txt;
    #  GTEST_/IMGUI_/MILO_WGPU_CALL_COUNT arrive through the linked targets.)

    # dc3-owned static libraries milo-tests links (nativeui, songbrowser_ui, ...).
    libs = []
    def walk(tid, seen):
        # sorted by name: the file API's dependency order is not stable across
        # reconfigures, and the output must be (a regen with no dc3 change is a no-op)
        for dep in sorted(by_id[tid].get("dependencies", []), key=lambda x: by_id[x["id"]]["name"]):
            d = by_id[dep["id"]]
            if d["name"] == ENGINE_TARGET or d["name"] in seen:
                continue
            seen.add(d["name"])
            walk(dep["id"], seen)
            libs.append(d)
    walk(tgt["id"], set())
    lib_blocks = []
    for d in libs:
        srcs = [rel(s["path"]) for s in d["sources"] if not is_test_or_mechanics(s["path"])]
        if d["type"] != "STATIC_LIBRARY":
            continue
        if any(s is None for s in srcs):
            # not dc3 source (imgui is a FetchContent checkout); the engine build
            # must provide a target of that name
            lib_blocks.append((d["name"], None, None, None, None))
            continue
        cg = d["compileGroups"][0]
        linc = [rel(i["path"]) for i in cg.get("includes", []) if rel(i["path"])
                and not os.path.relpath(os.path.realpath(i["path"]), native_src).startswith("build")]
        lopts = [tok for f in _fragments(cg) for tok in f.split()]
        ldeps = sorted(by_id[x["id"]]["name"] for x in d.get("dependencies", []))
        lib_blocks.append((d["name"], srcs, linc, lopts, ldeps))

    head = subprocess.run(["git", "-C", dc3_root, "rev-parse", "--short=12", "HEAD"],
                          capture_output=True, text=True).stdout.strip() or "unknown"
    L = []
    w = L.append
    w("# " + "=" * 76)
    w("# dc3_runtime_sources.cmake  --  GENERATED by tests/gen_dc3_runtime_sources.py.")
    w("# Do not hand-edit: regenerate from a configured dc3-decomp native build:")
    w("#")
    w("#     python3 tests/gen_dc3_runtime_sources.py --build-dir <dc3>/native/build")
    w("#")
    w(f"# Source of truth: dc3-decomp target `{TARGET}` (CMake file API codemodel-v2),")
    w(f"# read at dc3 commit {head}: every source it compiles except its test .cpp")
    w("# (native/tests/) and PCH stubs, the per-source carve-outs it applies, its")
    w("# compile context, and the dc3-owned static libraries it links.")
    w("# tests/CMakeLists.txt refuses to configure if a listed file is missing.")
    w("# " + "=" * 76)
    w("")
    w(f"set(DC3_RUNTIME_GENERATED_FROM \"{head}\")")
    w("")
    w("set(DC3_RUNTIME_SOURCES")
    for s in sorted(runtime):
        w(f"    {s}")
    w(")")
    w("")
    w("# Per-source properties, exactly as dc3's milo-tests compiles these files.")
    w("macro(dc3_runtime_source_props)")
    for srcs, p in props:
        w("    set_source_files_properties(")
        for s in sorted(srcs):
            w(f"        {s}")
        w("        TARGET_DIRECTORY milo-engine-tests")
        w("        PROPERTIES")
        for k, v in p:
            w(f"            {k} \"{v}\"")
        w("    )")
    w("endmacro()")
    w("")
    w("set(DC3_RUNTIME_COMPILE_OPTIONS")
    for o in joined:
        w(f"    \"{o}\"")
    w(")")
    w("set(DC3_RUNTIME_COMPILE_DEFINITIONS")
    for d in runtime_defs:
        w(f"    {d}")
    w(")")
    w("set(DC3_RUNTIME_INCLUDE_DIRS")
    for i in incs:
        w(f"    {i}")
    w(")")
    w("set(DC3_RUNTIME_SYSTEM_INCLUDE_DIRS")
    for i in sys_incs:
        w(f"    {i}")
    w(")")
    w("")
    w("# dc3-owned static libraries milo-tests links, in dependency order.")
    owned = {b[0] for b in lib_blocks if b[1] is not None}
    w("set(DC3_RUNTIME_LIBRARY_SOURCES)")
    for name, srcs, linc, lopts, ldeps in lib_blocks:
        if srcs is None:
            continue
        w(f"set(DC3_RUNTIME_LIB_{name}_SOURCES")
        for src in srcs:
            w(f"    {src}")
        w(")")
        w(f"list(APPEND DC3_RUNTIME_LIBRARY_SOURCES ${{DC3_RUNTIME_LIB_{name}_SOURCES}})")
    w("set(DC3_RUNTIME_LIBRARIES)")
    w("macro(dc3_runtime_libraries)")
    for name, srcs, linc, lopts, ldeps in lib_blocks:
        if srcs is None:
            w(f"    if(NOT TARGET {name})")
            w(f"        message(FATAL_ERROR \"milo-engine-tests: dc3's {TARGET} links `{name}`, which is not dc3 source;\"")
            w(f"            \" the engine build must provide a target of that name.\")")
            w("    endif()")
            w(f"    list(APPEND DC3_RUNTIME_LIBRARIES {name})")
            continue
        lname = f"dc3rt_{name}"
        w(f"    add_library({lname} STATIC ${{DC3_RUNTIME_LIB_{name}_SOURCES}})")
        if linc:
            w(f"    target_include_directories({lname} PUBLIC")
            for i in linc:
                w(f"        {i}")
            w("    )")
        if lopts:
            w(f"    target_compile_options({lname} PRIVATE {' '.join(lopts)})")
        w(f"    target_compile_features({lname} PUBLIC cxx_std_17)")
        for dn in ldeps:
            w(f"    target_link_libraries({lname} PUBLIC {'dc3rt_' + dn if dn in owned else dn})")
        w(f"    list(APPEND DC3_RUNTIME_LIBRARIES {lname})")
    w("endmacro()")
    w("")
    open(args.out, "w").write("\n".join(L))
    print(f"gen: wrote {args.out}: {len(runtime)} runtime sources, {len(props)} carve-out groups, "
          f"{len(lib_blocks)} libraries, from dc3 {head}", file=sys.stderr)


if __name__ == "__main__":
    main()
