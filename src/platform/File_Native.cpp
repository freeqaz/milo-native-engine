// DC3 Native Port - File I/O Implementation
// Replaces File_Win.cpp - POSIX file operations

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "os/Archive.h"
#include "os/Debug.h"
#include "os/File.h"
#include "os/System.h"

// Configurable data directory for native port (where gen/, config/ etc. live)
static char gNativeDataDir[512] = ".";

// Optional overlay directory — files here shadow the archive/data dir.
// Used for native-only DTA patches (e.g. adding settings UI toggles).
static char gNativeOverlayDir[512] = "";

void NativeSetDataDir(const char *dir) {
    strncpy(gNativeDataDir, dir, sizeof(gNativeDataDir) - 1);
    gNativeDataDir[sizeof(gNativeDataDir) - 1] = '\0';
}

const char *NativeGetDataDir() { return gNativeDataDir; }

void NativeSetOverlayDir(const char *dir) {
    strncpy(gNativeOverlayDir, dir, sizeof(gNativeOverlayDir) - 1);
    gNativeOverlayDir[sizeof(gNativeOverlayDir) - 1] = '\0';
}

const char *NativeGetOverlayDir() { return gNativeOverlayDir; }

// Content drives. On the 360 a mounted content package is a device of its own:
// XContentCreate mounts it as `cntNNNNNNNN:` and every path into it carries
// that drive (`cnt00000003:/songs/milkshake/milkshake.mid`). The game depends
// on the drive being there, not only on the files resolving: File_Win's
// FileIsLocal() treats any drive path as local, and data reads it too -- the
// `song` ObjectDir type's change_subdir handler (world/world_objects.dta)
// sends a downloaded song's `../../world/shared/director.milo` subdir back to
// the disc's copy ONLY when the path has a ':' in it. A content manager maps
// each mounted package's drive to the host directory that holds it, and
// unmaps it at unmount, as XContentClose does.
namespace {
struct NativeDrive {
    char name[32];
    char hostDir[512];
};
NativeDrive gNativeDrives[16];
int gNumNativeDrives = 0;

// The host directory for `path`'s drive, or null when the path has no drive
// or its drive is not mapped. `rest` gets the path inside the drive.
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
} // namespace

bool NativeMountDrive(const char *drive, const char *hostDir) {
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

void NativeUnmountDrive(const char *drive) {
    for (int i = 0; i < gNumNativeDrives; i++) {
        if (strcasecmp(gNativeDrives[i].name, drive) == 0) {
            gNativeDrives[i] = gNativeDrives[--gNumNativeDrives];
            return;
        }
    }
}

// Check if a file exists in the overlay directory
static bool NativeOverlayExists(const char *file) {
    if (!gNativeOverlayDir[0] || !file || !*file) return false;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/%s", gNativeOverlayDir, file);
    struct stat st;
    return stat(buf, &st) == 0;
}

// Is `file` a LOOSE file -- present on the filesystem but absent from the
// mounted archive index? That is how real DLC ships (loose files inside an
// STFS container, never in the .ark), and how mods and hand-authored assets
// arrive too.
//
// ★ ORDER IS LOAD-BEARING AND IS THE WHOLE SAFETY ARGUMENT: the archive is
// consulted FIRST and WINS. So every path the ark contains keeps resolving
// through ArkFile and reading exactly the bytes it read before -- this
// function can only ever return true for a path the ark does NOT have, and
// for such a path the pre-existing behaviour is a GUARANTEED HARD FAILURE
// (NewFile -> ArkFile -> Fail() -> delete -> null). There is no case where
// this converts a working load into a different working load.
//
// The stat therefore lands only on what is otherwise an error path, so the
// cost is not paid by ordinary disc content.
static bool NativeLooseFileExists(const char *file) {
    if (!TheArchive) return false;
    // Normalize the way the archive index is keyed (drops "./", collapses
    // "..", etc). FileMakePathBuf, not FileMakePath: the latter returns a
    // shared static buffer and asserts MainThread().
    char norm[256];
    FileMakePathBuf(".", file, norm);
    int arkNum = 0, fileSize = 0, ucSize = 0;
    unsigned long long byteOff = 0;
    if (TheArchive->GetFileInfo(norm, arkNum, byteOff, fileSize, ucSize))
        return false; // the archive owns it -- not loose, and the ark wins
    // Qualify exactly as every other read here does, so NativeSetDataDir()
    // and the overlay directory are both honoured.
    char qualified[256];
    FileQualifiedFilename(qualified, 0x100, file);
    struct stat st;
    return stat(qualified, &st) == 0 && S_ISREG(st.st_mode);
}

// On Xbox, FileIsLocal checks for drive letters (d: = disc = not local).
// On native, files without absolute paths are "not local" when UsingCD,
// so they get routed through the archive system (ArkFile).
// Files that exist in the overlay directory are treated as local so they
// bypass the archive and load from disk.
bool FileIsLocal(const char *file) {
    if (!file || !*file) return true;
    // Absolute paths are always local
    if (file[0] == '/') return true;
    // So is a path on a mounted content drive, as File_Win's drive rule has it.
    if (NativeDriveHostDir(file, nullptr)) return true;
    // Files in overlay directory are local (bypass archive)
    if (NativeOverlayExists(file)) return true;
    // Loose files (DLC/mods) are local too -- see NativeLooseFileExists.
    // Gated on UsingCD() because that is the only mode in which the caller
    // (os/File.cpp NewFile) would otherwise build an ArkFile.
    if (UsingCD() && NativeLooseFileExists(file)) return true;
    // When using CD (archive), relative paths are archive files, not local
    return false;
}

int FileGetStat(const char *iFilename, FileStat *iBuffer) {
    String fullName;
    FileQualifiedFilename(fullName, iFilename);
    struct stat st;
    if (stat(fullName.c_str(), &st) != 0) return -1;
    iBuffer->st_mode = st.st_mode;
    iBuffer->st_size = st.st_size;
#ifdef __APPLE__
    iBuffer->st_ctime = st.st_ctimespec.tv_sec;
    iBuffer->st_atime = st.st_atimespec.tv_sec;
    iBuffer->st_mtime = st.st_mtimespec.tv_sec;
#else
    iBuffer->st_ctime = st.st_ctim.tv_sec;
    iBuffer->st_atime = st.st_atim.tv_sec;
    iBuffer->st_mtime = st.st_mtim.tv_sec;
#endif
    return 0;
}

int FileDelete(const char *iFilename) {
    String str;
    FileQualifiedFilename(str, iFilename);
    return unlink(str.c_str()) == 0 ? 0 : -1;
}

int FileMkDir(const char *iDirname) {
    String str;
    FileQualifiedFilename(str, iDirname);
    return mkdir(str.c_str(), 0755) == 0 ? 1 : 0;
}

void FileQualifiedFilename(char *out, int outSize, const char *in) {
    MILO_ASSERT(in && out, 0x121);
    const char *driveRest;
    if (const char *hostDir = NativeDriveHostDir(in, &driveRest)) {
        snprintf(out, outSize > 0 ? outSize : 0x100, "%s/%s", hostDir, driveRest);
        return;
    }
    // On native, prepend the data directory (like Xbox prepends "d:")
    // If the file exists in the overlay directory, use that instead.
    String str(in);
    const char *inStr = str.c_str();
    char buf[256];
    const char *baseDir = NativeOverlayExists(inStr) ? gNativeOverlayDir : gNativeDataDir;
    const char *path = FileMakePathBuf(baseDir, inStr, buf);
    strcpy(out, path);
}

void FileEnumerate(
    const char *dir,
    void (*cb)(const char *, const char *),
    bool recurse,
    const char *pattern,
    bool b2
) {
    if (UsingCD() && TheArchive) {
        TheArchive->Enumerate(dir, cb, recurse, pattern);
        return;
    }

    char qualified[256];
    FileQualifiedFilename(qualified, 0x100, dir);

    DIR *d = opendir(qualified);
    if (!d) {
        MILO_LOG("FileEnumerate: cannot open %s\n", qualified);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Mirror the Win32 reference (DC3 src/system/os/File_Win.cpp:107-111)
        // exactly: when the qualified directory is ".", the path built here is
        // the *bare* filename, not "./name".
        //
        // `buf` is not just a stat target -- it is what FileMatch() prefix-
        // matches against the caller's pattern, and what the recursion descends
        // into. FileQualifiedFilename() prepends gNativeDataDir (default "."),
        // so an unconditional "%s/%s" made `buf` data-dir-qualified while the
        // caller's pattern was not, and FileMatch requires a literal prefix
        // match up to the first wildcard. Every pattern not beginning with a
        // wildcard therefore enumerated *empty rather than failing* -- a silent
        // wrong answer, not a crash. Callers that hit it: DC3's
        // ShaderProgram.cpp:53 ("%s/shaders/*.fx") and obj/Utl.cpp:482,517,
        // plus RecursePatternInternal, whose #ifndef HX_NATIVE guard was
        // removed on 2026-08-19 and which had no other platform primitive.
        char buf[512];
        if (strcmp(qualified, ".") == 0) {
            snprintf(buf, sizeof(buf), "%s", entry->d_name);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", qualified, entry->d_name);
        }

        struct stat st;
        if (stat(buf, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (b2 && (!pattern || FileMatch(buf, pattern))) {
                cb(qualified, entry->d_name);
            }
            if (recurse) {
                FileEnumerate(buf, cb, recurse, pattern, b2);
            }
        } else {
            if (!b2 && (!pattern || FileMatch(buf, pattern))) {
                cb(qualified, entry->d_name);
            }
        }
    }
    closedir(d);
}
