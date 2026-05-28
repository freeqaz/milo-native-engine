// CDReader Stub (Web)
// The web port bypasses the .ark archive system entirely.
// Files are served individually by the dev server's HTTP API and stored
// in Emscripten's MEMFS under /data/.  The engine opens them via regular
// POSIX fopen/fread (routed through MEMFS), so CDRead is never called.

#ifdef __EMSCRIPTEN__

#include "os/CDReader.h"
#include <cstdio>

bool CDReadDone() { return true; }

int CDRead(int arkFile, int offset, int size, void *buffer) {
    // Should not be called on web — archive system is bypassed
    printf("CDReader_Web: CDRead called (not supported on web)\n");
    return 1;  // error
}

bool NativeArkRead(int arkFile, long long byteOffset, void *buffer, int bytes) {
    printf("CDReader_Web: NativeArkRead called (not supported on web)\n");
    return false;
}

bool CDReadExternal(void *&v, int arkFile, unsigned long long byteOffset) {
    printf("CDReader_Web: CDReadExternal called (not supported on web)\n");
    return false;
}

#endif // __EMSCRIPTEN__
