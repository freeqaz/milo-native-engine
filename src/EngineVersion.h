#pragma once

// milo-native-engine — shared LP64 modern-C++ runtime.
//
// This is the deliverable engine consumed by each Milo decomp's native build
// (dc3-decomp, rb3 Wii, rb3-xenon) via add_subdirectory(). The engine is
// game-agnostic: it never includes per-game headers (hamobj/, band3/, dance/)
// or per-platform SDK headers (xdk/, revolution/). See README.md and the
// canonical roadmap at rb3/docs/native/NATIVE_PORT_ROADMAP.md.
//
// Phase 0.1: this header backs the single placeholder translation unit so
// libmilo-engine.a is a valid, linkable archive before the gfx/audio/platform/
// stl/system subsystems land in Phase 0.2.

namespace milo {

// Version string of the engine source tree. Each decomp pins a known-good
// engine commit (MILO_ENGINE_PIN); this string is a human-facing companion to
// that SHA and lets a linked executable confirm which engine it bound against.
const char* EngineVersion();

}  // namespace milo
