# milo-native-engine — agent guide

Shared LP64 modern-C++ runtime that native ports of three Milo-engine decomps
link against: **dc3-decomp** (DC3, Xbox 360 / MSVC-PPC), **rb3** (RB3 Wii / MWCC),
**rb3-xenon** (RB3 Xbox 360 / MSVC-PPC). See `README.md` for the full picture.

## Canonical plan (lives in a sibling repo, not here)

- Roadmap + phase tracker: `../rb3/docs/native/NATIVE_PORT_ROADMAP.md`
- What moves into the engine, in what shape: `../rb3/docs/native/NATIVE_PORT_INVENTORY.md`
- The model being extracted: `../dc3-decomp/native/` + `../dc3-decomp/docs/native/NATIVE_PORT_STATUS.md`

## Load-bearing build model (understand before touching CMake)

The engine `.cpp` files include the **consuming decomp's** Milo headers (obj/,
rndobj/, math/…) and compile with that decomp's matched-fork compat flags. The
engine is **not standalone-compilable** — it compiles inside a consumer's
context. A consumer injects `MILO_ENGINE_DECOMP_INCLUDE_DIRS` / `_COMPAT_FLAGS`
/ `_PCH` before `add_subdirectory`. With no context →
`MILO_ENGINE_HAVE_CONTEXT=OFF` → only the `EngineVersion.cpp` placeholder builds.
For standalone reference builds use `cmake -C cmake/dc3-reference.cmake`.

Pin handling: `MILO_ENGINE_PIN` is a sticky `CACHE STRING`; after bumping it in a
consumer's CMakeLists, re-read with `cmake -UMILO_ENGINE_PIN -B build` once.

## Commit conventions

- **Do not include `Co-Authored-By` lines in commit messages.** This matches the
  sibling decomp repos (`rb3/CLAUDE.md`, `dc3-decomp/CLAUDE.md`).
- A background permuter continuously rewrites `../dc3-decomp/src/system/**` and
  `../rb3/src/**` for asm-match. When committing in those repos, always stage an
  explicit file whitelist (`git add <paths>`) — never `git add -A`/`git add .`.
- HX_NATIVE matched-fork edits are additive `#ifdef HX_NATIVE … #endif` blocks;
  the permuter never produces them, so they rarely conflict — re-read + re-apply
  if a file shifts under you.
- ★ **Land worktree branches with `git merge --no-ff`, never cherry-pick,
  squash, or `--ff-only`** (effective 2026-08-04; supersedes older cherry-pick /
  `format-patch` guidance in any repo). Rebase onto the default branch first,
  then merge with a real message saying what the work found and what it
  deliberately did not do. The intermediate commits — especially the reverts and
  their reasoning — are the point, and they feed the training-data pipelines.
  Cherry-pick survives only for salvaging one commit from an abandoned branch.

## Three-layer source model

| Layer | Lives in | Compiles under | On native link line? |
|-------|----------|----------------|----------------------|
| Matched fork | `<decomp>/src/system/**` | MWCC (RB3) / MSVC-PPC (DC3) | No — asm-match only |
| Engine runtime | `milo-native-engine/src/**` | Clang LP64 | Yes — the deliverable |
| Per-decomp glue | `<decomp>/native/src/**` | Clang LP64 | Yes — compat/SDK shims, link glue |
