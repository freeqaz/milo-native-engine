// milo-native-engine - Memory System (POSIX)
// SDK-agnostic implementation of the PhysicalFree / PhysicalUsage /
// PhysMemTypeTracker surface declared in the matched-fork Memory.h.
// Replaces the Xbox Memory_Xbox.cpp on native; pairs with PhysicalAlloc[Tracked]
// which the per-decomp Memory_Xbox.cpp (compiled there) still provides.
//
// Implementation note: PhysicalAlloc/Tracked are NOT defined here — on DC3
// today only PhysicalFree/PhysicalFreeTracked/PhysicalUsage and the
// PhysMemTypeTracker ctor/dtor are needed from the native-port .cpp; allocs go
// through the matched-fork XPhysicalAlloc path. We use plain free() — a thin
// no-op tracker tally hangs off this file for symmetry/future use.

#include "Memory.h"

#include <cstdlib>

void PhysicalFree(void *address) {
    free(address);
}

void PhysicalFreeTracked(void *address, const char *, int, const char *) {
    free(address);
}

int PhysicalUsage() {
    return 0;
}

// PhysMemTypeTracker is a scoped tracking helper on Xbox (records physical
// allocations into a named category). On native we have no physical-memory
// pool to track, so ctor/dtor are no-ops.
PhysMemTypeTracker::PhysMemTypeTracker(Symbol) {}
PhysMemTypeTracker::~PhysMemTypeTracker() {}
