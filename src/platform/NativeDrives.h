// Content drives, shared by File_Native.cpp (desktop) and File_Web.cpp (web).
// Exactly one of the two is linked into any binary; each includes this header
// once, so the table below has one instance per program.
//
// On the 360 a mounted content package is a device of its own: XContentCreate
// mounts it as `cntNNNNNNNN:` and every path into it carries that drive
// (`cnt00000003:/songs/milkshake/milkshake.mid`). The game depends on the drive
// being there, not only on the files resolving: File_Win's FileIsLocal() treats
// any drive path as local, and data reads it too -- the `song` ObjectDir type's
// change_subdir handler (world/world_objects.dta) sends a downloaded song's
// `../../world/shared/director.milo` subdir back to the disc's copy ONLY when
// the path has a ':' in it. A content manager maps each mounted package's drive
// to the directory that holds it (a host directory on desktop, a MEMFS
// directory on web, whose files are fetched on demand), and unmaps it at
// unmount, as XContentClose does. The public entry points are
// NativeMountDrive / NativeUnmountDrive, defined in each File_*.cpp.
#pragma once

#include <cstring>
#include <strings.h>

namespace {
struct NativeDrive {
    char name[32];
    char hostDir[512];
};
NativeDrive gNativeDrives[16];
int gNumNativeDrives = 0;

// The directory for `path`'s drive, or null when the path has no drive or its
// drive is not mapped. `rest` gets the path inside the drive.
const char *NativeDriveHostDir(const char *path, const char **rest) {
    if (!path || gNumNativeDrives == 0) return nullptr;
    const char *colon = strchr(path, ':');
    if (!colon) return nullptr;
    size_t len = colon - path;
    for (int i = 0; i < gNumNativeDrives; i++) {
        if (strlen(gNativeDrives[i].name) == len
            && strncasecmp(gNativeDrives[i].name, path, len) == 0) {
            if (rest) {
                const char *r = colon + 1;
                while (*r == '/' || *r == '\\') r++;
                *rest = r;
            }
            return gNativeDrives[i].hostDir;
        }
    }
    return nullptr;
}

bool NativeDriveMap(const char *drive, const char *hostDir) {
    if (!drive || !*drive || !hostDir || !*hostDir) return false;
    if (strlen(drive) >= sizeof(gNativeDrives[0].name)
        || strlen(hostDir) >= sizeof(gNativeDrives[0].hostDir)) {
        return false;
    }
    NativeDrive *slot = nullptr;
    for (int i = 0; i < gNumNativeDrives; i++) {
        if (strcasecmp(gNativeDrives[i].name, drive) == 0) slot = &gNativeDrives[i];
    }
    if (!slot) {
        if (gNumNativeDrives == (int)(sizeof(gNativeDrives) / sizeof(gNativeDrives[0])))
            return false;
        slot = &gNativeDrives[gNumNativeDrives++];
    }
    strcpy(slot->name, drive);
    strcpy(slot->hostDir, hostDir);
    return true;
}

void NativeDriveUnmap(const char *drive) {
    for (int i = 0; i < gNumNativeDrives; i++) {
        if (strcasecmp(gNativeDrives[i].name, drive) == 0) {
            gNativeDrives[i] = gNativeDrives[--gNumNativeDrives];
            return;
        }
    }
}
} // namespace
