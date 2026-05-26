#include "EngineVersion.h"

// Overridable at configure time via target_compile_definitions; defaults here so
// a bare `cmake --build` still produces a meaningful string.
#ifndef MILO_ENGINE_VERSION
#define MILO_ENGINE_VERSION "0.1.0-phase0"
#endif

namespace milo {

const char* EngineVersion() { return MILO_ENGINE_VERSION; }

}  // namespace milo
